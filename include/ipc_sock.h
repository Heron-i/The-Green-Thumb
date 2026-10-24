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
 * only file that makes socket system calls.
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

#endif
