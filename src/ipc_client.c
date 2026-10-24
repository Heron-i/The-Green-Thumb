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
