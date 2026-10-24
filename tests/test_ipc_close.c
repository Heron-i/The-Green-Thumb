/* Unit tests for closing the IPC client (src/ipc_client.c):
 * gt_ipc_client_close.
 *
 * Purpose: prove close hands the socket ctx back exactly once (a second
 * destroy would be a double free, a missing one a leaked socket), is safe
 * on NULL, and sends nothing more, so buffered frames are dropped rather
 * than half-sent. Runs against tests/fake_sock.h. Run under make test /
 * asan / memcheck; Valgrind is what proves the client's own memory is
 * freed. Implements ICC-01 through ICC-03. ICC-03 uses gt_ipc_client_send
 * to leave a frame in the ring. Opening is in test_ipc_open.c.
 *
 * What these tests cannot prove: that the Linux destroy really closes the
 * file descriptor. That's src/ipc_sock.c, checked by test_ipc_loopback.c. */
#include <stddef.h>

#include "check.h"
#include "fake_sock.h"
#include "ipc.h"
#include "ipc_sock.h"

/* ---- ICC-01: destroy is called exactly once ----
 * Whether or not the client ever connected. */
static void test_icc01_destroy_called_once(void)
{
    fake_sock fake;
    gt_ipc_client *client = gt_ipc_client_open_ops(fake_sock_ops(&fake), &fake);
    CHECK(client != NULL);

    gt_ipc_client_close(client);
    CHECK_EQ(fake.destroy_calls, 1);
    CHECK_EQ(fake.send_calls, 0);
}

/* ---- ICC-02: close(NULL) is safe ----
 * Like free(NULL): a caller whose open failed can still close
 * unconditionally on its exit path. Reaching the next line is the pass;
 * a crash fails the whole binary. */
static void test_icc02_close_null_is_safe(void)
{
    gt_ipc_client_close(NULL);
    CHECK(1);
}

/* ---- ICC-03: close with frames buffered sends nothing ----
 * One EAGAIN leaves a frame in the ring. close must not try to send it
 * (a send while shutting down could block or half-send); the frame is
 * dropped, the ctx is destroyed once, and Valgrind sees nothing leaked. */
static void test_icc03_close_with_buffered_sends_nothing(void)
{
    fake_sock fake;
    gt_ipc_client *client = gt_ipc_client_open_ops(fake_sock_ops(&fake), &fake);
    fake_sock_push_send_result(&fake, GT_ERR_FULL);
    gt_msg reading = {
        .type = GT_MSG_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .counter = 1, .seq = 0, .value = 100,
    };

    CHECK_EQ(gt_ipc_client_send(client, &reading, 0, NULL), GT_OK);
    CHECK_EQ(fake.n_sent, 0);
    int sends_before_close = fake.send_calls;

    gt_ipc_client_close(client);
    CHECK_EQ(fake.send_calls, sends_before_close);
    CHECK_EQ(fake.n_sent, 0);
    CHECK_EQ(fake.destroy_calls, 1);
}

static const test_case_t tests[] = {
    {"ICC-01", "destroy is called exactly once", test_icc01_destroy_called_once},
    {"ICC-02", "close(NULL) is safe", test_icc02_close_null_is_safe},
    {"ICC-03", "close with frames buffered sends nothing", test_icc03_close_with_buffered_sends_nothing},
};

int main(void)
{
    return RUN_TESTS(tests);
}
