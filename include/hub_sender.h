#ifndef HUB_SENDER_H
#define HUB_SENDER_H

#include <stdint.h>
#include "ipc.h"
#include "ipc_sock.h"
#include "log_record.h"

/* The hub's hop-2 sender: its connection to storage.sock (DESIGN.md
 * failure-mode table, Hub -> storage rows; Sensor-Pipeline.md, Hub
 * contract (hop 2)).
 *
 * Records from gt_hub_accept wait here, unstamped, in a 1024-slot queue
 * (about 17 min of readings) until storage takes them. On every connect
 * storage first sends its 8 B seq_resume reply, and nothing is sent until
 * it arrives. The sender then picks its next seq with gt_next_seq (the
 * max rule, PF-03) and stamps each record with seq + CRC just before
 * send(). A seq is used only when send() returns GT_OK, so a failed send
 * never leaves a hole in the numbering.
 *
 * States (the diagram is in Sensor-Pipeline.md):
 *   DISCONNECTED    connect when the backoff allows (100 ms -> 2 s, the
 *                   same GT_IPC_BACKOFF_* waits as the sensor client)
 *   AWAITING_RESUME connected; poll recv for the reply, for up to
 *                   GT_HUB_RESUME_WAIT_MS, sending nothing
 *   SENDING         drain the queue oldest first
 *
 * Model: pure logic over the gt_sock_ops seam, like ipc_client.c. The
 * time comes in as now_ms (monotonic), nothing blocks and nothing is
 * logged: events come back as flags and the hub logs them. */

#define GT_HUB_QUEUE_SLOTS    1024   /* 1024 records x 24 B, about 17 min */
#define GT_HUB_RESUME_WAIT_MS 2000   /* how long to wait for seq_resume */

/* Event flags, one bit each (the same pattern as GT_IPC_EV_*). The hub
 * logs the name in brackets. */
#define GT_HUB_SENDER_EV_UNREACHABLE 0x01u  /* lost storage, or couldn't reach it:
                                             * storage_unreachable, once per outage */
#define GT_HUB_SENDER_EV_RECONNECTED 0x02u  /* seq_resume arrived after an outage:
                                             * storage_reconnected */
#define GT_HUB_SENDER_EV_RESUMED     0x04u  /* a valid seq_resume set next_seq:
                                             * seq_resume (on every connect) */
#define GT_HUB_SENDER_EV_BAD_REPLY   0x08u  /* a bad or wrong-size reply, or bytes
                                             * after it: ipc_bad_message */
#define GT_HUB_SENDER_EV_QUEUE_FULL  0x10u  /* the queue is full, records refused:
                                             * hub_queue_full, once per run */

/* Counters for tests and for log fields (named like gt_ipc_stats; the
 * function that fills it is gt_hub_sender_stats, and C can't give a type
 * and a function the same name). */
typedef struct {
    uint32_t queued;          /* records waiting now (0..GT_HUB_QUEUE_SLOTS) */
    uint32_t sent;            /* records storage's socket took, since open */
    uint32_t refused;         /* records refused because the queue was full */
    int      resumed;         /* 1 once a seq_resume reply has set next_seq */
    uint32_t next_seq;        /* the seq the next send will use (valid once resumed) */
    int      has_own;         /* 1 after the first successful send: the sender
                               * keeps its own counter from then on (PF-03) */
    uint32_t first_sent_seq;  /* the first seq sent (valid once has_own); with
                               * next_seq - 1 it is hub_stop's range A..B */
} gt_hub_send_stats;

typedef struct gt_hub_sender gt_hub_sender;

/* A sender over any socket ops: tests pass tests/fake_sock.h, and
 * gt_hub_sender_open uses the Linux ops. Does not connect: storage that
 * isn't up yet is not an error (Mechanism E). Ownership of ctx is the same
 * as gt_ipc_client_open_ops: once ops is non-NULL the sender owns ctx and
 * calls ops->destroy(ctx) exactly once, from gt_hub_sender_close or at
 * once if the sender can't be allocated (returns NULL). NULL ops return
 * NULL and leave ctx to the caller. */
gt_hub_sender *gt_hub_sender_open_ops(const gt_sock_ops *ops, void *ctx);

/* The real sender for storage.sock at `sock_path` (built in H7). NULL for
 * a bad path or no memory. */
gt_hub_sender *gt_hub_sender_open(const char *sock_path);

/* Queue one record from gt_hub_accept (seq is ignored: it is stamped at
 * send). Only queues; gt_hub_sender_pump does the sending.
 *   GT_OK        queued
 *   GT_ERR_FULL  refused and counted; a GAP record covers it (built in H6)
 *   GT_ERR_INVAL NULL sender/record, or a type / sensor_id
 *                gt_record_encode would reject; nothing queued
 * *events_out (set to 0 first; NULL to ignore) gets
 * GT_HUB_SENDER_EV_QUEUE_FULL on the first refusal of a run. */
gt_status gt_hub_sender_push(gt_hub_sender *sender, const gt_record *unstamped,
                             unsigned *events_out);

/* Do whatever the state allows at now_ms: connect if a retry is due, look
 * for the seq_resume reply, and send queued records. Never blocks. Losing
 * storage is not an error here; it's reported in *events_out (the
 * GT_HUB_SENDER_EV_* flags, set to 0 first; NULL to ignore). GT_OK, or
 * GT_ERR_INVAL for a NULL sender. */
gt_status gt_hub_sender_pump(gt_hub_sender *sender, uint64_t now_ms, unsigned *events_out);

/* Copy the counters into *stats_out. GT_ERR_INVAL for NULL args. */
gt_status gt_hub_sender_stats(const gt_hub_sender *sender, gt_hub_send_stats *stats_out);

/* Close the connection and free the sender. Sends nothing more: records
 * still queued are dropped (hub_main drains first and logs how many were
 * left). NULL is a no-op. */
void      gt_hub_sender_close(gt_hub_sender *sender);

#endif
