/* Unit tests for closing the IPC client (src/ipc_client.c):
 * gt_ipc_client_close.
 *
 * Purpose: prove close hands the socket ctx back exactly once (a second
 * destroy would be a double free, a missing one a leaked socket), is safe
 * on NULL, and sends nothing more, so buffered frames are dropped rather
 * than half-sent. Runs against tests/fake_sock.h. Run under make test /
 * asan / memcheck; Valgrind is what proves the client's own memory is
 * freed. Implements ICC-01 through ICC-03. ICC-03 needs a frame in the
 * ring, which only gt_ipc_client_send can put there, so it arrives with
 * Step 18. Opening is in test_ipc_open.c.
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

static const test_case_t tests[] = {
    {"ICC-01", "destroy is called exactly once", test_icc01_destroy_called_once},
    {"ICC-02", "close(NULL) is safe", test_icc02_close_null_is_safe},
};

int main(void)
{
    return RUN_TESTS(tests);
}
