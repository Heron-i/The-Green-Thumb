/* Linux AF_UNIX implementation of the gt_sock_ops seam (see ipc_sock.h).
 *
 * Purpose: this file is the only place that makes socket system calls. It
 * moves whole 16 B frames from a sensor process to hub.sock and turns what
 * the kernel says into a gt_status. It knows nothing about rings, backoff
 * or GAPs: that policy lives in the client (ipc_client.c), which reaches
 * this file only through the ops table, so tests can swap in
 * tests/fake_sock.h.
 *
 * Linux model: the socket is AF_UNIX SOCK_SEQPACKET, so the kernel keeps
 * message boundaries: one send() is one whole message and the hub's recv()
 * gets exactly that message. Every call is non-blocking (SOCK_NONBLOCK),
 * so a slow or frozen hub returns EAGAIN instead of stalling the 1 Hz
 * sampler, and send() passes MSG_NOSIGNAL, so a dead hub is the error
 * EPIPE instead of a SIGPIPE signal that would kill the sensor process.
 * How errno maps to a gt_status:
 *   connect: any failure (ENOENT: no socket file; ECONNREFUSED: file but
 *            no hub listening; EAGAIN: the hub's backlog is full) -> GT_ERR_IO
 *   send:    EAGAIN -> GT_ERR_FULL; EPIPE, ECONNRESET, anything else -> GT_ERR_IO
 *   recv:    EAGAIN -> GT_OK with 0 bytes; peer closed (0), ECONNRESET,
 *            anything else -> GT_ERR_IO
 * The same file serves the hub's storage sender on storage.sock: it sends
 * 24 B records and uses recv for the seq_resume reply. */
#include "ipc_sock.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Per-client state, passed to every op as the opaque `ctx`. The address is
 * built once in gt_ipc_sock_linux and reused by every reconnect.
 * `struct sockaddr_un` is the AF_UNIX address type: a family field plus
 * the socket file's path in a fixed 108-byte array (sun_path). */
typedef struct {
    struct sockaddr_un addr;
    int                fd;     /* the connected socket, or -1 when not connected */
} linux_sock;

/* Open a fresh socket and connect it to the hub. connect() takes a generic
 * `struct sockaddr *`, so the AF_UNIX address goes through a cast; the
 * kernel reads its family field to know the real type. On a non-blocking
 * AF_UNIX socket, connect either finishes at once or fails at once, so
 * there's nothing to wait for. Any failure closes the new fd and is
 * GT_ERR_IO, with no retry here, not even on EINTR: the client retries
 * after its backoff wait. Already connected is GT_OK and changes nothing.
 * SOCK_CLOEXEC closes the fd automatically if this process ever exec()s
 * another program, so the socket can't leak into it. */
static gt_status sock_connect(void *ctx)
{
    linux_sock *sock = ctx;
    if (sock->fd >= 0) {
        return GT_OK;
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return GT_ERR_IO;
    }
    if (connect(fd, (const struct sockaddr *)&sock->addr, sizeof sock->addr) != 0) {
        close(fd);
        return GT_ERR_IO;
    }
    sock->fd = fd;
    return GT_OK;
}

/* Send one whole frame. A SEQPACKET send is all or nothing, so anything
 * but exactly `len` bytes is a failure. EINTR (a signal arrived before
 * anything was sent) is simply tried again. EAGAIN means the hub is alive
 * but hasn't recv()d fast enough, so its buffer is full: GT_ERR_FULL, and
 * the client keeps the frame. Everything else, including EPIPE and
 * ECONNRESET when the hub is gone, is GT_ERR_IO. Not connected is
 * GT_ERR_IO too. */
static gt_status sock_send(void *ctx, const uint8_t *buf, size_t len)
{
    linux_sock *sock = ctx;
    if (sock->fd < 0) {
        return GT_ERR_IO;
    }
    ssize_t sent;
    /* Repeat only while a signal interrupted the call before it sent anything. */
    do {
        sent = send(sock->fd, buf, len, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);

    if (sent == (ssize_t)len) {
        return GT_OK;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return GT_ERR_FULL;
    }
    return GT_ERR_IO;
}

/* Receive one whole message without waiting. MSG_DONTWAIT makes this one
 * call non-blocking even on its own (the socket already is). EINTR is
 * tried again, as in sock_send. EAGAIN means nothing has arrived yet:
 * GT_OK with *len_out = 0, and the caller polls again later. recv()
 * returning 0 means the peer closed the connection; that, ECONNRESET and
 * not being connected are all GT_ERR_IO. A message longer than cap comes
 * back cut to cap bytes (SEQPACKET), which is why callers pass one spare
 * byte. *len_out is untouched on GT_ERR_IO. */
static gt_status sock_recv(void *ctx, uint8_t *buf, size_t cap, size_t *len_out)
{
    linux_sock *sock = ctx;
    if (sock->fd < 0) {
        return GT_ERR_IO;
    }
    ssize_t received;
    /* Repeat only while a signal interrupted the call before anything arrived. */
    do {
        received = recv(sock->fd, buf, cap, MSG_DONTWAIT);
    } while (received < 0 && errno == EINTR);

    if (received > 0) {
        *len_out = (size_t)received;
        return GT_OK;
    }
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        *len_out = 0;
        return GT_OK;
    }
    return GT_ERR_IO;
}

/* Close the current connection but keep the ctx and its address, so the
 * next connect makes a fresh socket. Called by the client when send says
 * the hub is gone. Safe when not connected. */
static void sock_disconnect(void *ctx)
{
    linux_sock *sock = ctx;
    if (sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
    }
}

/* Close any connection and free the ctx. gt_ipc_client_close calls this
 * exactly once; the ctx must not be used after. */
static void sock_destroy(void *ctx)
{
    sock_disconnect(ctx);
    free(ctx);
}

/* The real implementation of the seam. The functions above are static, so
 * the rest of the program can reach them only through this table. */
static const gt_sock_ops linux_ops = {
    .connect    = sock_connect,
    .send       = sock_send,
    .recv       = sock_recv,
    .disconnect = sock_disconnect,
    .destroy    = sock_destroy,
};

/* Prepare a ctx for the hub socket at `path`; no socket is opened yet.
 * The path must fit in sun_path with its closing '\0', so its length must
 * be under sizeof addr.sun_path (108): GT_ERR_INVAL otherwise, or for NULL
 * args or an empty path. GT_ERR_NOMEM if the ctx can't be allocated. The
 * address is checked and built before anything is allocated, so every
 * failure leaves the outputs untouched with nothing to undo. */
gt_status gt_ipc_sock_linux(const char *path, const gt_sock_ops **ops_out, void **ctx_out)
{
    if (path == NULL || ops_out == NULL || ctx_out == NULL) {
        return GT_ERR_INVAL;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    size_t path_len = strlen(path);
    if (path_len == 0 || path_len >= sizeof addr.sun_path) {
        return GT_ERR_INVAL;
    }
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);

    linux_sock *sock = malloc(sizeof *sock);
    if (sock == NULL) {
        return GT_ERR_NOMEM;
    }
    sock->addr = addr;
    sock->fd = -1;

    *ops_out = &linux_ops;
    *ctx_out = sock;
    return GT_OK;
}
