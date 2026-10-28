#ifndef FAKE_SOCK_H
#define FAKE_SOCK_H

/* Fake gt_sock_ops for IPC client and hub sender tests: no socket, no
 * peer, no waiting. Sender tests also script recv() replies (seq_resume)
 * and set expected_len to 24.
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
#define FAKE_SOCK_MAX_REPLIES 16
/* Largest frame the fake keeps: the hub sender's 24 B record (GT_REC_SIZE
 * in log_record.h). The sensor client's 16 B messages fit in the front. */
#define FAKE_SOCK_MAX_FRAME   24

/* One scripted recv() outcome. result GT_OK with len 0 is "nothing has
 * arrived yet"; GT_OK with len > 0 delivers bytes[0..len-1]; GT_ERR_IO is
 * "storage closed the connection". */
typedef struct {
    gt_status result;
    uint8_t   bytes[FAKE_SOCK_MAX_FRAME];
    size_t    len;
} fake_sock_reply;

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

    /* Every frame send() returned GT_OK for, in order: what the peer got.
     * sent_len[i] is that frame's length (16 for a message, 24 for a
     * record); a longer frame keeps only its first FAKE_SOCK_MAX_FRAME. */
    uint8_t  sent[FAKE_SOCK_MAX_SENT][FAKE_SOCK_MAX_FRAME];
    size_t   sent_len[FAKE_SOCK_MAX_SENT];
    size_t   n_sent;
    /* The frame length a correct caller sends; 0 (what fake_sock_ops
     * leaves) means GT_MSG_SIZE, the sensor client's. Hub sender tests set
     * it to 24. */
    size_t   expected_len;

    /* recv(): scripted replies, used front to back, one per call; once
     * they run out every recv returns "nothing has arrived yet". */
    fake_sock_reply recv_replies[FAKE_SOCK_MAX_REPLIES];
    size_t   n_recv_replies;
    size_t   recv_reply_pos;
    int      recv_calls;

    int      disconnect_calls;
    int      destroy_calls;

    /* Client bugs the fake can see; a correct client leaves these at 0. */
    int      connect_while_connected;
    int      send_while_disconnected;
    int      wrong_len_sends;   /* a send() whose len wasn't the expected length */
    int      recv_while_disconnected;
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

/* Script the outcome of a later recv() call; outcomes are used in the
 * order they're pushed.
 *   fake_sock_push_recv(fake, GT_OK, reply, 8)  delivers the 8 bytes
 *   fake_sock_push_recv(fake, GT_OK, NULL, 0)   nothing has arrived yet
 *   fake_sock_push_recv(fake, GT_ERR_IO, NULL, 0)  the peer closed
 * Bytes past FAKE_SOCK_MAX_FRAME are dropped, and so are pushes past the
 * cap. */
static inline void fake_sock_push_recv(fake_sock *fake, gt_status result,
                                       const uint8_t *bytes, size_t len)
{
    if (fake->n_recv_replies >= FAKE_SOCK_MAX_REPLIES) {
        return;
    }
    fake_sock_reply *reply = &fake->recv_replies[fake->n_recv_replies++];
    reply->result = result;
    reply->len = len < FAKE_SOCK_MAX_FRAME ? len : FAKE_SOCK_MAX_FRAME;
    if (bytes != NULL && reply->len > 0) {
        memcpy(reply->bytes, bytes, reply->len);
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
 * GT_OK send is recorded in sent[] (with its length in sent_len[]),
 * because only then did the kernel take the frame. A len other than
 * expected_len (GT_MSG_SIZE when 0) counts as wrong_len_sends. A GT_ERR_IO
 * result doesn't disconnect the fake by itself: the caller is expected to
 * call disconnect(), and a send after that counts as
 * send_while_disconnected. */
static inline gt_status fake_sock_send(void *ctx, const uint8_t *buf, size_t len)
{
    fake_sock *fake = ctx;
    fake->send_calls++;
    if (!fake->connected) {
        fake->send_while_disconnected++;
        return GT_ERR_IO;
    }
    size_t expected_len = fake->expected_len != 0 ? fake->expected_len : GT_MSG_SIZE;
    if (len != expected_len) {
        fake->wrong_len_sends++;
    }

    gt_status result = GT_OK;
    if (fake->send_result_pos < fake->n_send_results) {
        result = fake->send_results[fake->send_result_pos++];
    }
    if (result == GT_OK && fake->n_sent < FAKE_SOCK_MAX_SENT) {
        size_t copy_len = len < FAKE_SOCK_MAX_FRAME ? len : FAKE_SOCK_MAX_FRAME;
        memcpy(fake->sent[fake->n_sent], buf, copy_len);
        fake->sent_len[fake->n_sent] = len;
        fake->n_sent++;
    }
    return result;
}

/* Play the next scripted recv() outcome ("nothing has arrived yet" once
 * the script runs out). Delivered bytes are cut to `cap`, as SEQPACKET
 * cuts a message too long for the buffer, and *len_out is the number
 * copied. Like send, a GT_ERR_IO outcome doesn't disconnect the fake by
 * itself, and a recv while disconnected counts as recv_while_disconnected.
 * *len_out is untouched on GT_ERR_IO. */
static inline gt_status fake_sock_recv(void *ctx, uint8_t *buf, size_t cap, size_t *len_out)
{
    fake_sock *fake = ctx;
    fake->recv_calls++;
    if (!fake->connected) {
        fake->recv_while_disconnected++;
        return GT_ERR_IO;
    }
    if (fake->recv_reply_pos >= fake->n_recv_replies) {
        *len_out = 0;
        return GT_OK;
    }
    const fake_sock_reply *reply = &fake->recv_replies[fake->recv_reply_pos++];
    if (reply->result != GT_OK) {
        return reply->result;
    }
    size_t copy_len = reply->len < cap ? reply->len : cap;
    memcpy(buf, reply->bytes, copy_len);
    *len_out = copy_len;
    return GT_OK;
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
        .recv       = fake_sock_recv,
        .disconnect = fake_sock_disconnect,
        .destroy    = fake_sock_destroy,
    };
    memset(fake, 0, sizeof *fake);
    return &ops;
}

#endif
