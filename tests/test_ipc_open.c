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
#include <string.h>

#include "check.h"
#include "fake_sock.h"
#include "ipc.h"
#include "ipc_sock.h"

/* Size of sun_path in struct sockaddr_un on Linux, written out here so
 * the test states the limit rather than borrowing it from the code under
 * test. */
#define GT_IPC_TEST_SUN_PATH 108

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

/* ---- ICO-03: open(path) rejects NULL, "" and 108 B ----
 * A socket address holds the path in sun_path, a 108-byte array that
 * must also fit the closing '\0'. So 107 characters is the longest usable
 * path, and a 108-character one can never be connected to: open refuses
 * it up front rather than failing on every retry. */
static void test_ico03_open_rejects_bad_paths(void)
{
    char too_long[GT_IPC_TEST_SUN_PATH + 1];   /* 108 chars + '\0' */
    memset(too_long, 'a', GT_IPC_TEST_SUN_PATH);
    too_long[GT_IPC_TEST_SUN_PATH] = '\0';

    CHECK(gt_ipc_client_open(NULL) == NULL);
    CHECK(gt_ipc_client_open("") == NULL);
    CHECK(gt_ipc_client_open(too_long) == NULL);
}

/* ---- ICO-04: open(path) works with no socket there ----
 * The hub isn't up, so nothing exists at the path. Open still returns a
 * handle (it doesn't connect), and the first pump's failed connect
 * (ENOENT) is quiet: GT_OK and no event, as ICR-01 showed over the fake. */
static void test_ico04_open_without_hub(void)
{
    gt_ipc_client *client = gt_ipc_client_open("/tmp/gt-ico-no-such-dir/hub.sock");
    CHECK(client != NULL);

    unsigned events = 0xFFu;
    CHECK_EQ(gt_ipc_client_pump(client, 0, &events), GT_OK);
    CHECK_EQ(events, 0);

    gt_ipc_client_close(client);
}

static const test_case_t tests[] = {
    {"ICO-01", "open_ops returns a handle, no connect", test_ico01_open_ops_does_not_connect},
    {"ICO-02", "NULL ops are rejected", test_ico02_null_ops_rejected},
    {"ICO-03", "open(path) rejects NULL, \"\" and 108 B", test_ico03_open_rejects_bad_paths},
    {"ICO-04", "open(path) works with no socket there", test_ico04_open_without_hub},
};

int main(void)
{
    return RUN_TESTS(tests);
}
