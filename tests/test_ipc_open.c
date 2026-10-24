/* Unit tests for opening the IPC client (src/ipc_client.c):
 * gt_ipc_client_open_ops and gt_ipc_client_open.
 *
 * Purpose: prove that opening a client only sets it up. It must not try
 * to connect, because a hub that isn't up yet is not an error (DESIGN.md
 * Mechanism E: processes tolerate unavailable peers at startup); the
 * first send or pump makes the first attempt. Bad arguments give NULL.
 * ICO-01 and ICO-02 run against tests/fake_sock.h. ICO-03 and ICO-04
 * open by path, so they need the Linux socket layer (src/ipc_sock.c) and
 * arrive with it in Step 21. Run under make test / asan / memcheck.
 * Implements ICO-01 through ICO-04. Closing is in test_ipc_close.c.
 *
 * What these tests cannot prove: that a later connect really reaches a
 * hub. That's test_ipc_loopback.c, over a real socket. */
#include <stddef.h>

#include "check.h"
#include "fake_sock.h"
#include "ipc.h"
#include "ipc_sock.h"

/* ---- ICO-01: open_ops returns a handle, no connect ----
 * Opening touches nothing: no connect attempt and no send. */
static void test_ico01_open_ops_does_not_connect(void)
{
    fake_sock fake;
    gt_ipc_client *client = gt_ipc_client_open_ops(fake_sock_ops(&fake), &fake);

    CHECK(client != NULL);
    CHECK_EQ(fake.connect_calls, 0);
    CHECK_EQ(fake.send_calls, 0);
    CHECK_EQ(fake.destroy_calls, 0);

    gt_ipc_client_close(client);
}

/* ---- ICO-02: NULL ops are rejected ----
 * With no ops table there is no destroy to call, so the client can't take
 * ownership of the ctx: it returns NULL and leaves the ctx alone. */
static void test_ico02_null_ops_rejected(void)
{
    fake_sock fake;
    fake_sock_ops(&fake);   /* only to zero the counters */

    CHECK(gt_ipc_client_open_ops(NULL, &fake) == NULL);
    CHECK_EQ(fake.connect_calls, 0);
    CHECK_EQ(fake.destroy_calls, 0);
}

static const test_case_t tests[] = {
    {"ICO-01", "open_ops returns a handle, no connect", test_ico01_open_ops_does_not_connect},
    {"ICO-02", "NULL ops are rejected", test_ico02_null_ops_rejected},
};

int main(void)
{
    return RUN_TESTS(tests);
}
