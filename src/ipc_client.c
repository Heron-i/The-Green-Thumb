/* IPC client: a sensor process's connection to the hub (hub.sock).
 *
 * Purpose: get each 16 B message to the hub without ever blocking the
 * sampler, and without losing a reading silently when the hub is down or
 * restarting (DESIGN.md failure-mode table, Sensor -> hub row). Messages
 * wait in a fixed 64-slot ring until the hub takes them. While the hub is
 * unreachable the client retries with a doubling backoff (100 ms -> 2 s).
 * If the ring fills, new readings are refused and counted, and one GAP
 * message later tells the hub exactly which readings are missing.
 *
 * Model: pure logic over the gt_sock_ops seam (ipc_sock.h). This file makes
 * no system calls and reads no clock: the time comes in as now_ms, so tests
 * drive every path with tests/fake_sock.h. The client never logs either; it
 * returns GT_IPC_EV_* flags and the caller logs them. The rules are in
 * Sensor-Pipeline.md -> "IPC client: connection and ring rules". */
#include "ipc.h"
#include "ipc_sock.h"

#include <stdlib.h>
#include <string.h>

/* The ring is a circular queue: `head` is the slot of the oldest frame and
 * `count` is how many frames follow it, wrapping from the last slot back to
 * slot 0. A frame is the 16 bytes gt_msg_encode produced, so the ring holds
 * exactly what goes on the wire.
 *
 * `uint8_t ring[64][16]` is a two-dimensional array: 64 rows of 16 bytes,
 * stored back to back in one block. ring[slot] is one whole frame. Its
 * size is fixed when compiling, so the client never allocates per message. */
struct gt_ipc_client {
    const gt_sock_ops *ops;
    void              *ctx;

    uint8_t  ring[GT_IPC_RING_SLOTS][GT_MSG_SIZE];
    size_t   head;
    size_t   count;

    /* Connection and backoff. had_trouble is set by a lost connection or a
     * failed attempt, and makes the next successful connect report
     * GT_IPC_EV_RECONNECT; a clean first connect reports nothing. */
    int      connected;
    int      had_trouble;
    uint64_t next_attempt_ms;
    uint32_t backoff_ms;

    /* The GAP being built while readings are refused. gap_pending stays set
     * from the first refusal until the GAP is queued, so one overflow run
     * gives one GAP and one GT_IPC_EV_OVERFLOW. */
    int      gap_pending;
    uint8_t  gap_sensor_id;
    uint32_t gap_first_counter;
    uint32_t gap_count;

    gt_ipc_stats stats;
};

/* Copy one encoded frame into the slot just after the newest one.
 * (head + count) % GT_IPC_RING_SLOTS is that slot: `%` (remainder) wraps
 * an index that runs past the last slot back round to slot 0. GT_ERR_FULL
 * if all slots are taken; the ring is unchanged then. */
static gt_status ring_push(gt_ipc_client *client, const uint8_t frame[GT_MSG_SIZE])
{
    if (client->count == GT_IPC_RING_SLOTS) {
        return GT_ERR_FULL;
    }
    size_t tail = (client->head + client->count) % GT_IPC_RING_SLOTS;
    memcpy(client->ring[tail], frame, GT_MSG_SIZE);
    client->count++;
    client->stats.queued = (uint32_t)client->count;
    return GT_OK;
}

/* After a failed connect or a lost hub: no attempt before now + the
 * current wait, and double the wait for next time, up to the cap. Marks
 * the trouble so the next successful connect reports GT_IPC_EV_RECONNECT.
 * The waits run 100, 200, 400, 800, 1600, then 2000 ms each. */
static void schedule_retry(gt_ipc_client *client, uint64_t now_ms)
{
    client->had_trouble = 1;
    client->next_attempt_ms = now_ms + client->backoff_ms;
    client->backoff_ms *= 2;
    if (client->backoff_ms > GT_IPC_BACKOFF_MAX_MS) {
        client->backoff_ms = GT_IPC_BACKOFF_MAX_MS;
    }
}

/* Make sure there's a connection, trying at most once per retry window.
 * Returns 1 if connected (already, or just now), 0 if not: either the
 * retry isn't due yet (no attempt at all) or the attempt failed. On
 * success the wait goes back to 100 ms, and if a loss or failed attempt
 * came before, GT_IPC_EV_RECONNECT is added to *events. */
static int try_connect(gt_ipc_client *client, uint64_t now_ms, unsigned *events)
{
    if (client->connected) {
        return 1;
    }
    if (now_ms < client->next_attempt_ms) {
        return 0;
    }
    if (client->ops->connect(client->ctx) != GT_OK) {
        schedule_retry(client, now_ms);
        return 0;
    }
    client->connected = 1;
    client->backoff_ms = GT_IPC_BACKOFF_MIN_MS;
    if (client->had_trouble) {
        client->had_trouble = 0;
        client->stats.reconnects++;
        *events |= GT_IPC_EV_RECONNECT;
    }
    return 1;
}

/* Connect if needed, then hand frames to the socket oldest first until the
 * ring is empty or the socket says stop. A frame leaves the ring only once
 * send returns GT_OK, i.e. the kernel has taken it. Any other result keeps
 * that frame at the head for the next drain:
 *   GT_ERR_FULL (EAGAIN): the hub is alive but its buffer is full; stay
 *                         connected and just wait.
 *   GT_ERR_IO (EPIPE / ECONNRESET): the hub is gone; disconnect, report
 *                         GT_IPC_EV_DISCONNECT, and retry after the wait. */
static void drain(gt_ipc_client *client, uint64_t now_ms, unsigned *events)
{
    if (!try_connect(client, now_ms, events)) {
        return;
    }
    /* Invariant: ring[head] is the oldest frame the kernel hasn't taken. */
    while (client->count > 0) {
        gt_status result = client->ops->send(client->ctx, client->ring[client->head], GT_MSG_SIZE);
        if (result == GT_ERR_FULL) {
            return;
        }
        if (result != GT_OK) {
            client->ops->disconnect(client->ctx);
            client->connected = 0;
            *events |= GT_IPC_EV_DISCONNECT;
            schedule_retry(client, now_ms);
            return;
        }
        client->head = (client->head + 1) % GT_IPC_RING_SLOTS;
        client->count--;
        client->stats.queued = (uint32_t)client->count;
        client->stats.sent++;
    }
}

/* Queue the GAP message for the run of refused readings, and end the run.
 * Only called with at least 2 free slots, so the push can't fail and the
 * reading that follows it fits too. The encode can't fail either: the
 * sensor_id came from a reading that already encoded. */
static void push_gap(gt_ipc_client *client)
{
    gt_msg gap = {
        .type = GT_MSG_GAP, .sensor_id = client->gap_sensor_id,
        .counter = client->gap_first_counter, .seq = 0, .value = client->gap_count,
    };
    uint8_t frame[GT_MSG_SIZE];
    if (gt_msg_encode(&gap, frame) == GT_OK) {
        ring_push(client, frame);
    }
    client->gap_pending = 0;
}

/* Put one reading's frame in the ring, or refuse it into the current gap.
 * A reading is refused when the ring is full, and also while a gap is
 * pending but fewer than 2 slots are free: the GAP must go in just before
 * the next accepted reading, so the hub sees old data, then the GAP, then
 * new data. The first refusal of a run starts the gap (its sensor, its
 * counter) and reports GT_IPC_EV_OVERFLOW; later ones only add to the
 * count. Every refusal is counted in stats.refused. */
static gt_status enqueue_reading(gt_ipc_client *client, const gt_msg *message,
                                 const uint8_t frame[GT_MSG_SIZE], unsigned *events)
{
    if (client->gap_pending && GT_IPC_RING_SLOTS - client->count >= 2) {
        push_gap(client);
    }
    /* With no gap pending, try the push; with one still pending, refuse. */
    if (!client->gap_pending && ring_push(client, frame) == GT_OK) {
        return GT_OK;
    }
    if (!client->gap_pending) {
        client->gap_pending = 1;
        client->gap_sensor_id = message->sensor_id;
        client->gap_first_counter = message->counter;
        client->gap_count = 0;
        *events |= GT_IPC_EV_OVERFLOW;
    }
    client->gap_count++;
    client->stats.refused++;
    return GT_ERR_FULL;
}

/* Set up a client over any socket ops; no connect yet (the first send or
 * pump tries straight away, because next_attempt_ms starts at 0).
 * calloc is malloc that also zeroes the memory, so every counter, flag and
 * ring slot starts at 0 without setting each one. Once ops is known the
 * client owns ctx, so a failed allocation destroys it before returning
 * NULL; with NULL ops there is no destroy to call and the caller keeps ctx. */
gt_ipc_client *gt_ipc_client_open_ops(const gt_sock_ops *ops, void *ctx)
{
    if (ops == NULL) {
        return NULL;
    }
    gt_ipc_client *client = calloc(1, sizeof *client);
    if (client == NULL) {
        ops->destroy(ctx);
        return NULL;
    }
    client->ops = ops;
    client->ctx = ctx;
    client->backoff_ms = GT_IPC_BACKOFF_MIN_MS;
    return client;
}

/* Hand the ctx back to the socket layer exactly once (destroy also closes
 * any open connection), then free the client. Nothing more is sent, so
 * frames still in the ring are dropped. NULL is a no-op, like free(NULL). */
void gt_ipc_client_close(gt_ipc_client *client)
{
    if (client == NULL) {
        return;
    }
    client->ops->destroy(client->ctx);
    free(client);
}

/* Encode `message`, queue it behind anything already waiting, and send
 * what the socket will take. The message is encoded first, so a bad one is
 * refused (GT_ERR_INVAL) before it takes a slot or causes a connect. The
 * ring is drained before the push as well as after: getting older frames
 * out first can free the slot this reading needs. */
gt_status gt_ipc_client_send(gt_ipc_client *client, const gt_msg *message,
                             uint64_t now_ms, unsigned *events_out)
{
    if (events_out != NULL) {
        *events_out = 0;
    }
    if (client == NULL || message == NULL) {
        return GT_ERR_INVAL;
    }

    uint8_t frame[GT_MSG_SIZE];
    if (gt_msg_encode(message, frame) != GT_OK) {
        return GT_ERR_INVAL;
    }
    unsigned events = 0;
    drain(client, now_ms, &events);
    gt_status pushed = enqueue_reading(client, message, frame, &events);
    drain(client, now_ms, &events);
    if (events_out != NULL) {
        *events_out = events;
    }
    return pushed;
}

/* The connect-and-drain half of send, with no new message: the sensor
 * loop calls this every tick, so a ring filled while the hub was down
 * still empties even when the sensor is offline and sends nothing. */
gt_status gt_ipc_client_pump(gt_ipc_client *client, uint64_t now_ms, unsigned *events_out)
{
    if (events_out != NULL) {
        *events_out = 0;
    }
    if (client == NULL) {
        return GT_ERR_INVAL;
    }
    unsigned events = 0;
    drain(client, now_ms, &events);
    if (events_out != NULL) {
        *events_out = events;
    }
    return GT_OK;
}

/* Copy the counters out. `*stats_out = client->stats` is a struct
 * assignment: C copies every field of the struct in one statement, so the
 * caller gets a snapshot it can't use to change the client. */
gt_status gt_ipc_client_stats(const gt_ipc_client *client, gt_ipc_stats *stats_out)
{
    if (client == NULL || stats_out == NULL) {
        return GT_ERR_INVAL;
    }
    *stats_out = client->stats;
    return GT_OK;
}
