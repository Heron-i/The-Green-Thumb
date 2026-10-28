/* Unit tests for the soil loop's hub side (src/soil_loop.c): readings
 * reaching the hub through the IPC client while the hub comes and goes.
 *
 * Purpose: prove a reading taken while the hub is down is kept and
 * delivered by a later tick's pump (with no new reading due), and that
 * the client's disconnect / reconnect events are logged as ipc_disconnect
 * and ipc_reconnect. Runs against fake_i2c and fake_sock. Run under make
 * test / asan / memcheck. Implements SLI-01 and SLI-02.
 *
 * Not here: ring overflow and the GAP message. The client's own tests
 * (test_ipc_overflow.c) cover them, and 65 scripted readings don't fit in
 * fake_i2c's 64-byte read script. */
#include <stdint.h>

#include "check.h"
#include "soil_loop_harness.h"

/* ---- SLI-01: hub down: reading kept, pump delivers it later ----
 * The first connect fails, so the reading waits in the ring. 100 ms later
 * (the first backoff) a tick with no reading due connects and sends it,
 * and ipc_reconnect is logged because a failed attempt came before. */
static void test_sli01_pump_delivers_buffered(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    harness_push_raw(&harness, 400);
    harness.sock.fail_connects = 1;

    CHECK_EQ(gt_soil_loop_tick(harness.loop, 0), GT_OK);
    CHECK_EQ(harness.sock.n_sent, 0);

    CHECK_EQ(gt_soil_loop_tick(harness.loop, GT_IPC_BACKOFF_MIN_MS), GT_OK);
    CHECK_EQ(harness.sock.n_sent, 1);
    CHECK_EQ(harness_count(&harness, "ipc_reconnect"), 1);
    gt_msg message;
    CHECK_EQ(harness_sent(&harness, 0, &message), GT_OK);
    CHECK_EQ(message.value, 400);

    gt_soil_loop_destroy(harness.loop);
}

/* ---- SLI-02: hub lost and back: disconnect, reconnect logged ----
 * The second reading's send hits EPIPE: ipc_disconnect is logged and the
 * reading is kept; the next tick after the backoff resends it and logs
 * ipc_reconnect. */
static void test_sli02_disconnect_and_reconnect_logged(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    harness_push_raw(&harness, 400);
    harness_push_raw(&harness, 410);

    CHECK_EQ(gt_soil_loop_tick(harness.loop, 0), GT_OK);
    CHECK_EQ(harness.sock.n_sent, 1);

    fake_sock_push_send_result(&harness.sock, GT_ERR_IO);
    CHECK_EQ(gt_soil_loop_tick(harness.loop, HARNESS_PERIOD_MS), GT_OK);
    CHECK_EQ(harness_count(&harness, "ipc_disconnect"), 1);
    CHECK_EQ(harness.sock.n_sent, 1);

    CHECK_EQ(gt_soil_loop_tick(harness.loop, HARNESS_PERIOD_MS + GT_IPC_BACKOFF_MIN_MS), GT_OK);
    CHECK_EQ(harness_count(&harness, "ipc_reconnect"), 1);
    CHECK_EQ(harness.sock.n_sent, 2);
    gt_msg message;
    CHECK_EQ(harness_sent(&harness, 1, &message), GT_OK);
    CHECK_EQ(message.counter, 1);
    CHECK_EQ(message.value, 410);

    gt_soil_loop_destroy(harness.loop);
}

static const test_case_t tests[] = {
    {"SLI-01", "hub down: reading kept, pump delivers it",
     test_sli01_pump_delivers_buffered},
    {"SLI-02", "hub lost and back: both events logged",
     test_sli02_disconnect_and_reconnect_logged},
};

int main(void)
{
    return RUN_TESTS(tests);
}
