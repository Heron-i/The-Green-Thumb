/* The hub's hop-2 sender: records from the hub to storage.sock (see
 * hub_sender.h).
 *
 * Purpose: get every record the hub accepted to storage, in order, each
 * with a seq that continues storage's numbering (DESIGN.md failure-mode
 * table, Hub -> storage rows; Sensor-Pipeline.md, Hub contract). Records
 * wait unstamped in a 1024-slot queue. On each connect the sender waits
 * for storage's seq_resume reply before sending anything, picks its next
 * seq with gt_next_seq (PF-03), and stamps seq + CRC only just before
 * each send(), so a seq is used only when the send succeeds.
 *
 * Model: pure logic over the gt_sock_ops seam, like ipc_client.c, whose
 * backoff it shares (GT_IPC_BACKOFF_MIN_MS -> _MAX_MS). No system calls
 * and no clock: now_ms is passed in, so tests drive every path with
 * tests/fake_sock.h. Nothing is logged; GT_HUB_SENDER_EV_* flags go back
 * to the hub, which logs them. */
#include "hub_sender.h"
#include "storage_proto.h"

#include <stdlib.h>
#include <string.h>

/* The sender's three states (the diagram is in Sensor-Pipeline.md, Hub
 * contract). An enum gives names to a small set of int values. */
typedef enum {
    SENDER_DISCONNECTED,     /* no connection; connect when the backoff allows */
    SENDER_AWAITING_RESUME,  /* connected; waiting for seq_resume, sending nothing */
    SENDER_SENDING           /* next_seq is known; drain the queue */
} sender_state;

/* The queue is a circular queue of unstamped records, as in ipc_client.c:
 * `head` is the slot of the oldest record and `count` how many follow it,
 * wrapping from the last slot back to slot 0. A gt_record is 24 B in
 * memory (with padding), so the queue is the 24 KB DESIGN.md budgets. */
struct gt_hub_sender {
    const gt_sock_ops *ops;
    void              *ctx;

    gt_record queue[GT_HUB_QUEUE_SLOTS];
    size_t    head;
    size_t    count;

    sender_state state;
    /* had_trouble is set when an outage is reported (storage_unreachable)
     * and cleared by the next valid reply (storage_reconnected), so each
     * outage is reported once however many attempts it takes. */
    int       had_trouble;
    uint64_t  next_attempt_ms;
    uint32_t  backoff_ms;
    uint64_t  resume_deadline_ms;   /* AWAITING_RESUME gives up at this time */

    gt_hub_send_stats stats;
};

/* After any failure to reach storage: report storage_unreachable if this
 * is the first failure of an outage, then allow no attempt before now +
 * the current wait and double the wait for next time, up to the cap. The
 * waits run 100, 200, 400, 800, 1600, then 2000 ms each, as for the
 * sensor client. */
static void schedule_retry(gt_hub_sender *sender, uint64_t now_ms, unsigned *events)
{
    if (!sender->had_trouble) {
        sender->had_trouble = 1;
        *events |= GT_HUB_SENDER_EV_UNREACHABLE;
    }
    sender->next_attempt_ms = now_ms + sender->backoff_ms;
    sender->backoff_ms *= 2;
    if (sender->backoff_ms > GT_IPC_BACKOFF_MAX_MS) {
        sender->backoff_ms = GT_IPC_BACKOFF_MAX_MS;
    }
}

/* Give up on the current connection: close it (the ctx stays, so the next
 * connect makes a fresh socket), go back to DISCONNECTED and schedule the
 * retry. Queued records stay queued and unstamped. */
static void drop_connection(gt_hub_sender *sender, uint64_t now_ms, unsigned *events)
{
    sender->ops->disconnect(sender->ctx);
    sender->state = SENDER_DISCONNECTED;
    schedule_retry(sender, now_ms, events);
}

/* DISCONNECTED: connect if the retry is due. On success move to
 * AWAITING_RESUME with GT_HUB_RESUME_WAIT_MS to get the reply. The backoff
 * is not reset here but only by a valid reply, so a storage that accepts
 * and never answers is retried at a doubling wait, not every 100 ms. */
static void try_connect(gt_hub_sender *sender, uint64_t now_ms, unsigned *events)
{
    if (now_ms < sender->next_attempt_ms) {
        return;
    }
    if (sender->ops->connect(sender->ctx) != GT_OK) {
        schedule_retry(sender, now_ms, events);
        return;
    }
    sender->state = SENDER_AWAITING_RESUME;
    sender->resume_deadline_ms = now_ms + GT_HUB_RESUME_WAIT_MS;
}

/* AWAITING_RESUME: look for storage's reply without waiting.
 *   - storage closed the connection:        drop it (storage_unreachable)
 *   - nothing yet:                          keep waiting until the deadline,
 *                                           then drop it
 *   - not exactly GT_RESUME_SIZE bytes, a reply gt_resume_decode rejects,
 *     or one with no next seq (UINT32_MAX): ipc_bad_message, drop it
 *   - a valid reply: next_seq = gt_next_seq(has_own, own next, reply), the
 *     wait goes back to 100 ms, and the sender moves to SENDING
 * The buffer is one byte bigger than the reply, so a longer message shows
 * up as too long instead of being cut to 8 bytes. While has_own is 1,
 * stats.next_seq is the sender's own next seq (1 + the last seq sent). */
static void await_resume(gt_hub_sender *sender, uint64_t now_ms, unsigned *events)
{
    uint8_t reply[GT_RESUME_SIZE + 1];
    size_t reply_len = 0;
    if (sender->ops->recv(sender->ctx, reply, sizeof reply, &reply_len) != GT_OK) {
        drop_connection(sender, now_ms, events);
        return;
    }
    if (reply_len == 0) {
        if (now_ms >= sender->resume_deadline_ms) {
            drop_connection(sender, now_ms, events);
        }
        return;
    }
    gt_resume resume;
    uint32_t next_seq = 0;
    if (reply_len != GT_RESUME_SIZE || gt_resume_decode(reply, &resume) != GT_OK ||
        gt_next_seq(sender->stats.has_own, sender->stats.next_seq, &resume, &next_seq) != GT_OK) {
        *events |= GT_HUB_SENDER_EV_BAD_REPLY;
        drop_connection(sender, now_ms, events);
        return;
    }
    sender->stats.next_seq = next_seq;
    sender->stats.resumed = 1;
    sender->state = SENDER_SENDING;
    sender->backoff_ms = GT_IPC_BACKOFF_MIN_MS;
    *events |= GT_HUB_SENDER_EV_RESUMED;
    if (sender->had_trouble) {
        sender->had_trouble = 0;
        *events |= GT_HUB_SENDER_EV_RECONNECTED;
    }
}

/* SENDING: storage sends nothing after its reply, so look once per pump
 * for anything it did send. Nothing waiting is the normal case: returns 1.
 * A closed connection (recv GT_ERR_IO) is caught here even when no record
 * is queued, so an idle hub reports storage_unreachable when it happens.
 * Any bytes at all are ipc_bad_message. Either way the connection is
 * dropped and 0 is returned, so nothing is sent on it. */
static int storage_stayed_quiet(gt_hub_sender *sender, uint64_t now_ms, unsigned *events)
{
    uint8_t unexpected[GT_RESUME_SIZE + 1];
    size_t unexpected_len = 0;
    gt_status result = sender->ops->recv(sender->ctx, unexpected, sizeof unexpected,
                                         &unexpected_len);
    if (result == GT_OK && unexpected_len == 0) {
        return 1;
    }
    if (result == GT_OK) {
        *events |= GT_HUB_SENDER_EV_BAD_REPLY;
    }
    drop_connection(sender, now_ms, events);
    return 0;
}

/* SENDING: if storage stayed quiet, send queued records oldest first
 * until the queue is empty or the socket says stop. Each send stamps a
 * copy of the head record with next_seq and encodes it (the CRC covers
 * seq, so it can only be computed now); the queued record itself stays
 * unstamped. Only GT_OK uses the seq: the record leaves the queue,
 * next_seq moves on, and the first one sent starts the sender's own
 * counter (has_own, first_sent_seq). Anything else keeps the head and
 * next_seq as they were:
 *   GT_ERR_FULL (EAGAIN): storage is alive but behind; stay connected.
 *   GT_ERR_IO (EPIPE / ECONNRESET): storage is gone; drop the connection.
 * The encode can't fail, because push already test-encoded the record.
 * next_seq wraps after UINT32_MAX, which at 1 Hz is 136 years away; storage
 * would then reject the records as old (seq_rejected), not lose them
 * silently. */
static void drain(gt_hub_sender *sender, uint64_t now_ms, unsigned *events)
{
    if (!storage_stayed_quiet(sender, now_ms, events)) {
        return;
    }
    /* Invariant: queue[head] is the oldest record storage hasn't taken,
     * and every seq below stats.next_seq has been sent exactly once. */
    while (sender->count > 0) {
        gt_record stamped = sender->queue[sender->head];
        stamped.seq = sender->stats.next_seq;
        uint8_t wire[GT_REC_SIZE];
        gt_record_encode(&stamped, wire);
        gt_status result = sender->ops->send(sender->ctx, wire, sizeof wire);
        if (result == GT_ERR_FULL) {
            return;
        }
        if (result != GT_OK) {
            drop_connection(sender, now_ms, events);
            return;
        }
        if (!sender->stats.has_own) {
            sender->stats.has_own = 1;
            sender->stats.first_sent_seq = stamped.seq;
        }
        sender->stats.next_seq++;
        sender->stats.sent++;
        sender->head = (sender->head + 1) % GT_HUB_QUEUE_SLOTS;
        sender->count--;
        sender->stats.queued = (uint32_t)sender->count;
    }
}

/* Set up a sender over any socket ops; no connect yet (the first pump
 * tries straight away, because next_attempt_ms starts at 0). calloc zeroes
 * the queue, the counters and the state (0 is SENDER_DISCONNECTED). Once
 * ops is known the sender owns ctx, so a failed allocation destroys it;
 * with NULL ops there is no destroy to call and the caller keeps ctx. */
gt_hub_sender *gt_hub_sender_open_ops(const gt_sock_ops *ops, void *ctx)
{
    if (ops == NULL) {
        return NULL;
    }
    gt_hub_sender *sender = calloc(1, sizeof *sender);
    if (sender == NULL) {
        ops->destroy(ctx);
        return NULL;
    }
    sender->ops = ops;
    sender->ctx = ctx;
    sender->state = SENDER_DISCONNECTED;
    sender->backoff_ms = GT_IPC_BACKOFF_MIN_MS;
    return sender;
}

/* Queue one unstamped record behind the others. It is test-encoded first
 * (into a scratch buffer that is thrown away), so a record storage could
 * never decode is refused here instead of reaching the head of the queue
 * and blocking every record behind it. (head + count) % GT_HUB_QUEUE_SLOTS
 * is the slot just after the newest record: `%` wraps it back to 0. A
 * full queue refuses and counts; H6 adds the event and the GAP. */
gt_status gt_hub_sender_push(gt_hub_sender *sender, const gt_record *unstamped,
                             unsigned *events_out)
{
    if (events_out != NULL) {
        *events_out = 0;
    }
    if (sender == NULL || unstamped == NULL) {
        return GT_ERR_INVAL;
    }
    uint8_t scratch[GT_REC_SIZE];
    if (gt_record_encode(unstamped, scratch) != GT_OK) {
        return GT_ERR_INVAL;
    }
    if (sender->count == GT_HUB_QUEUE_SLOTS) {
        sender->stats.refused++;
        return GT_ERR_FULL;
    }
    size_t tail = (sender->head + sender->count) % GT_HUB_QUEUE_SLOTS;
    sender->queue[tail] = *unstamped;
    sender->count++;
    sender->stats.queued = (uint32_t)sender->count;
    return GT_OK;
}

/* Move through the states as far as now_ms allows: connect, then (in the
 * same pump) look for the reply, since storage sends it the moment it
 * accepts. Each step runs only if the one before left the sender in the
 * right state, so a failed connect stops here until the next pump, and a
 * reply that just arrived lets the queue drain in the same pump. */
gt_status gt_hub_sender_pump(gt_hub_sender *sender, uint64_t now_ms, unsigned *events_out)
{
    if (events_out != NULL) {
        *events_out = 0;
    }
    if (sender == NULL) {
        return GT_ERR_INVAL;
    }
    unsigned events = 0;
    if (sender->state == SENDER_DISCONNECTED) {
        try_connect(sender, now_ms, &events);
    }
    if (sender->state == SENDER_AWAITING_RESUME) {
        await_resume(sender, now_ms, &events);
    }
    if (sender->state == SENDER_SENDING) {
        drain(sender, now_ms, &events);
    }
    if (events_out != NULL) {
        *events_out = events;
    }
    return GT_OK;
}

/* Copy the counters out as one struct assignment, so the caller gets a
 * snapshot it can't use to change the sender. */
gt_status gt_hub_sender_stats(const gt_hub_sender *sender, gt_hub_send_stats *stats_out)
{
    if (sender == NULL || stats_out == NULL) {
        return GT_ERR_INVAL;
    }
    *stats_out = sender->stats;
    return GT_OK;
}

/* Hand the ctx back to the socket layer exactly once (destroy also closes
 * any connection), then free the sender. Queued records are dropped
 * unsent. NULL is a no-op. */
void gt_hub_sender_close(gt_hub_sender *sender)
{
    if (sender == NULL) {
        return;
    }
    sender->ops->destroy(sender->ctx);
    free(sender);
}
