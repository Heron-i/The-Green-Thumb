#ifndef FAKE_SOCK_H
#define FAKE_SOCK_H

/* Fake gt_sock_ops for IPC client tests: no socket, no hub, no waiting.
 *
 * A test declares a fake_sock on the stack, passes fake_sock_ops(&f) and &f
 * to gt_ipc_client_open_ops, scripts connect failures and send results,
 * then inspects what the client did: connect attempts, frames sent,
 * disconnects and the final destroy. The fake is the ctx and is owned by
 * the test, so destroy() only counts and never frees. fake_sock_ops()
 * zeroes the fake, so call it before scripting anything.
 *
 * Header-only (static inline) for the same reason as fake_i2c.h: the
 * Makefile links each test .c with src/ only, and inline keeps unused
 * helpers from tripping -Werror. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ipc_sock.h"

#define FAKE_SOCK_MAX_SENT    256   /* enough for two full rings plus GAPs */
#define FAKE_SOCK_MAX_RESULTS 64

typedef struct {
    /* connect(): the next fail_connects calls return GT_ERR_IO. */
    int      connect_calls;
    int      fail_connects;
    int      connected;

    /* send(): scripted results, used front to back, one per call; once
     * they run out every send returns GT_OK. */
    gt_status send_results[FAKE_SOCK_MAX_RESULTS];
    size_t    n_send_results;
    size_t    send_result_pos;
    int       send_calls;

    /* Every frame send() returned GT_OK for, in order: what the hub got. */
    uint8_t  sent[FAKE_SOCK_MAX_SENT][GT_MSG_SIZE];
    size_t   n_sent;

    int      disconnect_calls;
    int      destroy_calls;

    /* Client bugs the fake can see; a correct client leaves these at 0. */
    int      connect_while_connected;
    int      send_while_disconnected;
    int      wrong_len_sends;   /* a send() whose len wasn't GT_MSG_SIZE */
} fake_sock;

/* Script the result of a later send() call: results are used in the order
 * they're pushed. GT_ERR_FULL acts like EAGAIN (hub alive, buffer full),
 * GT_ERR_IO like EPIPE (hub gone). Extra pushes past the cap are ignored. */
static inline void fake_sock_push_send_result(fake_sock *fake, gt_status result)
{
    if (fake->n_send_results < FAKE_SOCK_MAX_RESULTS) {
        fake->send_results[fake->n_send_results++] = result;
    }
}

/* Count the attempt; fail it while fail_connects is above 0, otherwise the
 * fake is connected from now until disconnect(). */
static inline gt_status fake_sock_connect(void *ctx)
{
    fake_sock *fake = ctx;
    fake->connect_calls++;
    if (fake->connected) {
        fake->connect_while_connected++;
    }
    if (fake->fail_connects > 0) {
        fake->fail_connects--;
        return GT_ERR_IO;
    }
    fake->connected = 1;
    return GT_OK;
}

/* Return the next scripted result (GT_OK once the script runs out). Only a
 * GT_OK send is recorded in sent[], because only then did the kernel take
 * the frame. A GT_ERR_IO result doesn't disconnect the fake by itself: the
 * client is expected to call disconnect(), and a send after that counts as
 * send_while_disconnected. */
static inline gt_status fake_sock_send(void *ctx, const uint8_t *buf, size_t len)
{
    fake_sock *fake = ctx;
    fake->send_calls++;
    if (!fake->connected) {
        fake->send_while_disconnected++;
        return GT_ERR_IO;
    }
    if (len != GT_MSG_SIZE) {
        fake->wrong_len_sends++;
    }

    gt_status result = GT_OK;
    if (fake->send_result_pos < fake->n_send_results) {
        result = fake->send_results[fake->send_result_pos++];
    }
    if (result == GT_OK && fake->n_sent < FAKE_SOCK_MAX_SENT) {
        size_t copy_len = len < GT_MSG_SIZE ? len : GT_MSG_SIZE;
        memcpy(fake->sent[fake->n_sent], buf, copy_len);
        fake->n_sent++;
    }
    return result;
}

/* Drop the connection; the fake stays usable for the next connect(). */
static inline void fake_sock_disconnect(void *ctx)
{
    fake_sock *fake = ctx;
    fake->disconnect_calls++;
    fake->connected = 0;
}

/* Count only: the test owns the fake, so there is nothing to free. A
 * correct client calls this exactly once (ICC-01). */
static inline void fake_sock_destroy(void *ctx)
{
    fake_sock *fake = ctx;
    fake->destroy_calls++;
    fake->connected = 0;
}

/* Reset `fake` and return the ops that route every call to it. Pass
 * `fake` itself as the ctx: gt_ipc_client_open_ops(fake_sock_ops(&f), &f). */
static inline const gt_sock_ops *fake_sock_ops(fake_sock *fake)
{
    static const gt_sock_ops ops = {
        .connect    = fake_sock_connect,
        .send       = fake_sock_send,
        .disconnect = fake_sock_disconnect,
        .destroy    = fake_sock_destroy,
    };
    memset(fake, 0, sizeof *fake);
    return &ops;
}

#endif
