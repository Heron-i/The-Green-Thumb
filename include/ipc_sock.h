#ifndef IPC_SOCK_H
#define IPC_SOCK_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"

/* Socket seam for the IPC client, the same idea as gt_i2c_ops in i2c_bus.h.
 *
 * The client (src/ipc_client.c) talks to hub.sock only through these ops,
 * so tests can swap in tests/fake_sock.h and run every reconnect, backoff
 * and overflow path with no hub and no real waiting. src/ipc_sock.c is the
 * only file that makes client-side socket system calls; servers
 * (listen_sock.c, storage_server.c) make their own and are tested over
 * real sockets (Sensor-Pipeline.md Decisions, PF-16). The hub's storage
 * sender (hub_sender.c) uses the same seam for storage.sock.
 *
 * The socket is AF_UNIX SOCK_SEQPACKET: the kernel keeps message
 * boundaries, so one send() is one whole 16 B message. Every op is
 * non-blocking, so a slow or dead hub never stalls the sampler. */
typedef struct {
    /* Open a socket and connect to the hub. GT_OK, or GT_ERR_IO if the hub
     * isn't there (ENOENT / ECONNREFUSED): the client retries later. */
    gt_status (*connect)(void *ctx);
    /* Send one whole frame. GT_OK when the kernel took it; GT_ERR_FULL on
     * EAGAIN (the hub is alive but its buffer is full: keep the frame and
     * try again later); GT_ERR_IO on EPIPE / ECONNRESET (the hub is gone:
     * disconnect and reconnect). */
    gt_status (*send)(void *ctx, const uint8_t *buf, size_t len);
    /* Receive one whole message into buf (cap bytes, cap > 0), without
     * waiting. GT_OK with *len_out = the message's length, or *len_out = 0
     * when nothing has arrived yet (EAGAIN); GT_ERR_IO when the peer closed
     * the connection (recv returns 0), reset it, or there is no connection.
     * Only the hub's storage sender calls it, to read the 8 B seq_resume
     * reply; the sensor client never does (Sensor-Pipeline.md, Hub
     * contract). SEQPACKET cuts off whatever doesn't fit in buf, so pass
     * one byte more than the expected message: a longer one then shows up
     * as too long instead of being silently cut. */
    gt_status (*recv)(void *ctx, uint8_t *buf, size_t cap, size_t *len_out);
    /* Close the current connection but keep ctx, so connect() can be
     * called again. Safe to call when not connected. */
    void      (*disconnect)(void *ctx);
    /* Close any connection and free ctx. Called exactly once, by
     * gt_ipc_client_close. */
    void      (*destroy)(void *ctx);
} gt_sock_ops;

/* Linux impl: prepares a ctx for `path` (e.g. "/run/plantpi/hub.sock") but
 * does not connect yet, so a hub that isn't up is not an error here.
 * GT_ERR_INVAL for a NULL/empty path or one too long for sun_path (108 B);
 * GT_ERR_NOMEM if the ctx can't be allocated. Outputs are untouched on
 * failure.
 *
 * The outputs are pointer-to-pointer (`**`) because the function hands back
 * two pointers: `*ops_out` is set to the ops table and `*ctx_out` to the new
 * ctx. A plain `*` parameter could only change what the function's own copy
 * points at; `**` lets it change the caller's pointer. */
gt_status gt_ipc_sock_linux(const char *path, const gt_sock_ops **ops_out, void **ctx_out);

/* A client over any socket ops: gt_ipc_client_open uses the Linux ops, and
 * tests pass tests/fake_sock.h (like gt_light_open_bus for the I2C seam).
 * Does not connect yet. Once ops is non-NULL the client owns ctx and calls
 * ops->destroy(ctx) exactly once: from gt_ipc_client_close, or straight
 * away if the client can't be allocated (returns NULL). NULL ops also
 * return NULL, but the ctx is left alone: there's no destroy to call, so
 * the caller still owns it. Declared here, not in ipc.h, because ipc.h can't see
 * gt_sock_ops without the two headers including each other. */
gt_ipc_client *gt_ipc_client_open_ops(const gt_sock_ops *ops, void *ctx);

#endif
