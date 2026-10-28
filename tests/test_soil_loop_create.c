/* Unit tests for creating and destroying the soil loop (src/soil_loop.c):
 * gt_soil_loop_create, gt_soil_loop_destroy, and tick's NULL check.
 *
 * Purpose: prove create does no I/O, rejects a bad config without leaking
 * the client it was handed, and that destroy releases the sensor bus and
 * the client exactly once whether or not the sensor ever opened. Run under
 * make test / asan / memcheck, so leaks and double-closes also fail.
 * Implements SLC-01 through SLC-04. Sampling is test_soil_loop_sample.c,
 * health test_soil_loop_health.c, the hub side test_soil_loop_ipc.c. */
#include <stddef.h>

#include "check.h"
#include "soil_loop_harness.h"

/* ---- SLC-01: create does no I/O ----
 * No bus open and no hub connect until the first tick, so a process that
 * starts before the hub or with no sensor still starts. */
static void test_slc01_create_does_no_io(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    CHECK(harness.loop != NULL);
    CHECK_EQ(harness.bus.open_calls, 0);
    CHECK_EQ(harness.sock.connect_calls, 0);
    CHECK_EQ(harness.logs.count, 0);

    gt_soil_loop_destroy(harness.loop);
}

/* ---- SLC-02: a bad config is rejected, client still closed ----
 * create owns the client from the call on, so each rejection must destroy
 * it exactly once. A NULL client is rejected too. */
static void test_slc02_bad_config_rejected(void)
{
    fake_bus_opener opener = { .open_calls = 0 };
    log_capture logs = { .count = 0 };
    gt_soil_loop_cfg good = {
        .period_ms = HARNESS_PERIOD_MS,
        .bus_open = harness_bus_open, .bus_ctx = &opener,
        .log = harness_log, .log_ctx = &logs,
    };
    gt_soil_loop_cfg zero_period = good;
    zero_period.period_ms = 0;
    gt_soil_loop_cfg no_opener = good;
    no_opener.bus_open = NULL;
    gt_soil_loop_cfg no_log = good;
    no_log.log = NULL;
    const gt_soil_loop_cfg *bad_cfgs[] = { NULL, &zero_period, &no_opener, &no_log };

    /* Invariant: each pass hands create a fresh client on a freshly zeroed fake. */
    for (size_t cfg_index = 0; cfg_index < sizeof bad_cfgs / sizeof bad_cfgs[0]; cfg_index++) {
        fake_sock sock;
        gt_ipc_client *client = gt_ipc_client_open_ops(fake_sock_ops(&sock), &sock);
        CHECK(gt_soil_loop_create(bad_cfgs[cfg_index], client, 0) == NULL);
        CHECK_EQ(sock.destroy_calls, 1);
    }
    CHECK(gt_soil_loop_create(&good, NULL, 0) == NULL);
}

/* ---- SLC-03: destroy closes the open sensor and the client ---- */
static void test_slc03_destroy_closes_both(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    harness_push_raw(&harness, 400);
    CHECK_EQ(gt_soil_loop_tick(harness.loop, 0), GT_OK);
    CHECK_EQ(harness.bus.i2c.closed, 0);

    gt_soil_loop_destroy(harness.loop);
    CHECK_EQ(harness.bus.i2c.closed, 1);
    CHECK_EQ(harness.sock.destroy_calls, 1);
}

/* ---- SLC-04: never-opened sensor, NULL loop are safe ----
 * The sensor never opened: destroy closes only the client. destroy(NULL)
 * is a no-op and tick(NULL) is GT_ERR_INVAL. */
static void test_slc04_unopened_and_null_safe(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    harness.bus.fail_opens = 1;
    CHECK_EQ(gt_soil_loop_tick(harness.loop, 0), GT_OK);

    gt_soil_loop_destroy(harness.loop);
    CHECK_EQ(harness.bus.i2c.closed, 0);
    CHECK_EQ(harness.sock.destroy_calls, 1);

    gt_soil_loop_destroy(NULL);
    CHECK_EQ(gt_soil_loop_tick(NULL, 0), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"SLC-01", "create does no I/O", test_slc01_create_does_no_io},
    {"SLC-02", "bad config rejected, client still closed", test_slc02_bad_config_rejected},
    {"SLC-03", "destroy closes the open sensor and client", test_slc03_destroy_closes_both},
    {"SLC-04", "never-opened sensor, NULL loop are safe", test_slc04_unopened_and_null_safe},
};

int main(void)
{
    return RUN_TESTS(tests);
}
