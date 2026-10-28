/* Unit tests for the soil loop's failure handling (src/soil_loop.c):
 * offline/online, re-init after a replug, and a sensor missing at startup.
 *
 * Purpose: prove the DESIGN.md failure-mode rows for a sensor. 10 failed
 * reads in a row mark it offline; while offline it is probed with
 * gt_soil_reinit every 5 s instead of read on the 60 s schedule; a probe
 * that succeeds logs sensor_reinit then sensor_online and readings resume;
 * and a sensor that can't be opened at startup doesn't stop the process,
 * it starts offline and is retried every 5 s. Runs against fake_i2c and
 * fake_sock. Run under make test / asan / memcheck.
 * Implements SLH-01 through SLH-04.
 *
 * What these tests cannot prove: that a real replugged board answers the
 * reinit. That needs the Pi (pull the STEMMA cable). */
#include <stdint.h>

#include "check.h"
#include "soil_loop_harness.h"

/* Open the sensor with one good reading at slot 0 (sent as counter 0),
 * then tick slots 1..failures with every read failing. Returns the time
 * of the last failed slot. */
static uint64_t fail_slots(soil_harness *harness, int failures)
{
    harness_push_raw(harness, 400);
    gt_soil_loop_tick(harness->loop, 0);

    uint64_t now_ms = 0;
    /* Invariant: slot_index - 1 reads have failed before this pass. */
    for (int slot_index = 1; slot_index <= failures; slot_index++) {
        now_ms = (uint64_t)slot_index * HARNESS_PERIOD_MS;
        harness_fail_next_read(harness);
        gt_soil_loop_tick(harness->loop, now_ms);
    }
    return now_ms;
}

/* ---- SLH-01: offline only on the 10th failure in a row ----
 * 9 failures log i2c_read_error each but stay online; the 10th logs
 * sensor_offline, once. */
static void test_slh01_offline_after_ten_failures(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);

    uint64_t last_failure = fail_slots(&harness, GT_HEALTH_OFFLINE_AFTER - 1);
    CHECK_EQ(harness_count(&harness, "sensor_offline"), 0);
    CHECK_EQ(harness_count(&harness, GT_HEALTH_I2C_READ_ERROR), GT_HEALTH_OFFLINE_AFTER - 1);

    harness_fail_next_read(&harness);
    gt_soil_loop_tick(harness.loop, last_failure + HARNESS_PERIOD_MS);
    CHECK_EQ(harness_count(&harness, "sensor_offline"), 1);
    CHECK_EQ(harness.sock.n_sent, 1);   /* only slot 0's good reading */

    gt_soil_loop_destroy(harness.loop);
}

/* ---- SLH-02: offline: probed every 5 s, not read on schedule ----
 * A probe is a reinit (reset + ID check). A failed probe tries again 5 s
 * later; a slot that comes due while offline takes no reading. */
static void test_slh02_offline_probes_every_5s(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    uint64_t offline_at = fail_slots(&harness, GT_HEALTH_OFFLINE_AFTER);
    int reads_when_offline = harness.bus.i2c.read_calls;
    int writes_when_offline = harness.bus.i2c.write_calls;

    /* Still unplugged: every write is NACKed, so each probe fails at the reset. */
    harness.bus.i2c.fail_writes = 1000;
    gt_soil_loop_tick(harness.loop, offline_at + GT_HEALTH_RETRY_MS - 100);
    CHECK_EQ(harness.bus.i2c.write_calls, writes_when_offline);
    gt_soil_loop_tick(harness.loop, offline_at + GT_HEALTH_RETRY_MS);
    CHECK_EQ(harness.bus.i2c.write_calls, writes_when_offline + 1);
    gt_soil_loop_tick(harness.loop, offline_at + 2 * GT_HEALTH_RETRY_MS - 100);
    CHECK_EQ(harness.bus.i2c.write_calls, writes_when_offline + 1);
    gt_soil_loop_tick(harness.loop, offline_at + 2 * GT_HEALTH_RETRY_MS);
    CHECK_EQ(harness.bus.i2c.write_calls, writes_when_offline + 2);

    /* The next slot comes due while offline: no moisture read is attempted. */
    gt_soil_loop_tick(harness.loop, offline_at + HARNESS_PERIOD_MS);
    CHECK_EQ(harness.bus.i2c.read_calls, reads_when_offline);
    CHECK_EQ(harness.sock.n_sent, 1);   /* only slot 0's good reading */
    CHECK_EQ(harness_count(&harness, "sensor_online"), 0);

    gt_soil_loop_destroy(harness.loop);
}

/* ---- SLH-03: a good probe reinits, goes online, reads resume ----
 * Replugged: the probe's reinit succeeds, sensor_reinit is logged before
 * sensor_online, and the next slot's reading reaches the hub. */
static void test_slh03_replug_reinits_then_online(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    uint64_t offline_at = fail_slots(&harness, GT_HEALTH_OFFLINE_AFTER);

    harness_push_hw_id(&harness);
    gt_soil_loop_tick(harness.loop, offline_at + GT_HEALTH_RETRY_MS);
    int reinit = harness_find(&harness, "sensor_reinit");
    int online = harness_find(&harness, "sensor_online");
    CHECK(reinit >= 0);
    CHECK(online > reinit);
    CHECK_EQ(harness.sock.n_sent, 1);

    harness_push_raw(&harness, 820);
    gt_soil_loop_tick(harness.loop, offline_at + HARNESS_PERIOD_MS);
    CHECK_EQ(harness.sock.n_sent, 2);
    gt_msg message;
    CHECK_EQ(harness_sent(&harness, 1, &message), GT_OK);
    CHECK_EQ(message.counter, 1);
    CHECK_EQ(message.value, 820);

    gt_soil_loop_destroy(harness.loop);
}

/* ---- SLH-04: open failing at startup: offline, retried 5 s ----
 * No exit (that would be a restart storm): i2c_open_error and
 * sensor_offline are logged, the open is retried every 5 s, and once it
 * works sensor_online is logged and the next slot is read. */
static void test_slh04_open_failure_retried(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    harness.bus.fail_opens = 2;

    gt_soil_loop_tick(harness.loop, 0);
    CHECK_EQ(harness.bus.open_calls, 1);
    CHECK_EQ(harness_count(&harness, "i2c_open_error"), 1);
    CHECK_EQ(harness_count(&harness, "sensor_offline"), 1);

    gt_soil_loop_tick(harness.loop, GT_HEALTH_RETRY_MS - 100);
    CHECK_EQ(harness.bus.open_calls, 1);
    gt_soil_loop_tick(harness.loop, GT_HEALTH_RETRY_MS);
    CHECK_EQ(harness.bus.open_calls, 2);
    CHECK_EQ(harness_count(&harness, "i2c_open_error"), 2);
    CHECK_EQ(harness_count(&harness, "sensor_offline"), 1);   /* not repeated */

    gt_soil_loop_tick(harness.loop, 2 * GT_HEALTH_RETRY_MS);
    CHECK_EQ(harness.bus.open_calls, 3);
    CHECK_EQ(harness_count(&harness, "sensor_online"), 1);
    CHECK_EQ(harness.sock.n_sent, 0);

    harness_push_raw(&harness, 390);
    gt_soil_loop_tick(harness.loop, HARNESS_PERIOD_MS);
    CHECK_EQ(harness.sock.n_sent, 1);
    gt_msg message;
    CHECK_EQ(harness_sent(&harness, 0, &message), GT_OK);
    CHECK_EQ(message.counter, 0);

    gt_soil_loop_destroy(harness.loop);
}

static const test_case_t tests[] = {
    {"SLH-01", "offline only on the 10th failure in a row",
     test_slh01_offline_after_ten_failures},
    {"SLH-02", "offline: probed every 5 s, not on schedule",
     test_slh02_offline_probes_every_5s},
    {"SLH-03", "good probe reinits, online, reads resume",
     test_slh03_replug_reinits_then_online},
    {"SLH-04", "open failing at startup: offline, retried",
     test_slh04_open_failure_retried},
};

int main(void)
{
    return RUN_TESTS(tests);
}
