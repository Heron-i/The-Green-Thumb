/* Unit tests for the soil loop's sampling schedule (src/soil_loop.c):
 * gt_soil_loop_tick taking and sending readings.
 *
 * Purpose: prove the first tick opens the seesaw and sends a reading, that
 * readings follow the period exactly (one per slot, however often tick is
 * called), that missed slots are skipped with sample_overrun instead of
 * read in a burst, and that a failed read sends nothing and doesn't use up
 * a counter. Runs against fake_i2c and fake_sock. Run under make test /
 * asan / memcheck. Implements SLS-01 through SLS-04.
 *
 * What these tests cannot prove: real timing jitter. That's the 100 ms
 * tick in src/soil_sensor_main.c, measured on the Pi. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "soil_loop_harness.h"

/* ---- SLS-01: first tick opens, reads, sends the reading ----
 * The hub gets {DATA, SOIL, counter 0, seq 0, raw}: seq is 0 because only
 * the hub stamps it. */
static void test_sls01_first_tick_sends_reading(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    harness_push_raw(&harness, 400);

    CHECK_EQ(gt_soil_loop_tick(harness.loop, 0), GT_OK);
    CHECK_EQ(harness.bus.open_calls, 1);
    CHECK_EQ(harness.sock.n_sent, 1);

    gt_msg message;
    CHECK_EQ(harness_sent(&harness, 0, &message), GT_OK);
    CHECK_EQ(message.type, GT_MSG_DATA);
    CHECK_EQ(message.sensor_id, GT_SENSOR_SOIL);
    CHECK_EQ(message.counter, 0);
    CHECK_EQ(message.seq, 0);
    CHECK_EQ(message.value, 400);
    CHECK_EQ(harness.logs.count, 0);

    gt_soil_loop_destroy(harness.loop);
}

/* ---- SLS-02: one reading per period, however often tick runs ----
 * Ticking every 100 ms for two periods gives readings at 0, 60 s and
 * 120 s only, with counters 0, 1, 2. */
static void test_sls02_one_reading_per_period(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    harness_push_raw(&harness, 300);
    harness_push_raw(&harness, 500);
    harness_push_raw(&harness, 700);

    /* Invariant: after the tick at now_ms, one reading per slot <= now_ms was sent. */
    for (uint64_t now_ms = 0; now_ms <= 2 * HARNESS_PERIOD_MS; now_ms += 100) {
        CHECK_EQ(gt_soil_loop_tick(harness.loop, now_ms), GT_OK);
        if (now_ms == HARNESS_PERIOD_MS - 100) {
            CHECK_EQ(harness.sock.n_sent, 1);
        }
    }
    CHECK_EQ(harness.sock.n_sent, 3);

    const uint32_t expected_raw[] = { 300, 500, 700 };
    for (size_t sent_index = 0; sent_index < 3; sent_index++) {
        gt_msg message;
        CHECK_EQ(harness_sent(&harness, sent_index, &message), GT_OK);
        CHECK_EQ(message.counter, sent_index);
        CHECK_EQ(message.value, expected_raw[sent_index]);
    }

    gt_soil_loop_destroy(harness.loop);
}

/* ---- SLS-03: missed slots are skipped, not read in a burst ----
 * The process stalls from 0 to 190 s: the 60 s and 120 s slots are gone.
 * One reading is taken for the 180 s slot, sample_overrun logs
 * skipped=2, and the schedule stays anchored (next reading at 240 s). */
static void test_sls03_overrun_skips_missed_slots(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    harness_push_raw(&harness, 300);
    harness_push_raw(&harness, 500);
    harness_push_raw(&harness, 700);

    CHECK_EQ(gt_soil_loop_tick(harness.loop, 0), GT_OK);
    CHECK_EQ(gt_soil_loop_tick(harness.loop, 190000), GT_OK);
    CHECK_EQ(harness.sock.n_sent, 2);
    CHECK_EQ(harness_count(&harness, "sample_overrun"), 1);
    int overrun = harness_find(&harness, "sample_overrun");
    CHECK(overrun >= 0 && strcmp(harness.logs.fields[overrun], "skipped=2") == 0);

    CHECK_EQ(gt_soil_loop_tick(harness.loop, 239999), GT_OK);
    CHECK_EQ(harness.sock.n_sent, 2);
    CHECK_EQ(gt_soil_loop_tick(harness.loop, 240000), GT_OK);
    CHECK_EQ(harness.sock.n_sent, 3);

    gt_msg message;
    CHECK_EQ(harness_sent(&harness, 2, &message), GT_OK);
    CHECK_EQ(message.counter, 2);   /* skipped slots use no counter */

    gt_soil_loop_destroy(harness.loop);
}

/* ---- SLS-04: a failed read sends nothing, keeps the counter ----
 * Slot 1's read fails: i2c_read_error is logged and nothing is sent. Slot
 * 2's reading is counter 1, so the hub sees no counter gap for a reading
 * that never existed. */
static void test_sls04_failed_read_sends_nothing(void)
{
    soil_harness harness;
    harness_start(&harness, HARNESS_PERIOD_MS, 0);
    harness_push_raw(&harness, 400);
    CHECK_EQ(gt_soil_loop_tick(harness.loop, 0), GT_OK);

    harness_fail_next_read(&harness);
    CHECK_EQ(gt_soil_loop_tick(harness.loop, HARNESS_PERIOD_MS), GT_OK);
    CHECK_EQ(harness.sock.n_sent, 1);
    CHECK_EQ(harness_count(&harness, GT_HEALTH_I2C_READ_ERROR), 1);

    harness_push_raw(&harness, 450);
    CHECK_EQ(gt_soil_loop_tick(harness.loop, 2 * HARNESS_PERIOD_MS), GT_OK);
    gt_msg message;
    CHECK_EQ(harness_sent(&harness, 1, &message), GT_OK);
    CHECK_EQ(message.counter, 1);
    CHECK_EQ(message.value, 450);

    gt_soil_loop_destroy(harness.loop);
}

static const test_case_t tests[] = {
    {"SLS-01", "first tick opens, reads, sends the reading",
     test_sls01_first_tick_sends_reading},
    {"SLS-02", "one reading per period", test_sls02_one_reading_per_period},
    {"SLS-03", "missed slots skipped, not read in a burst",
     test_sls03_overrun_skips_missed_slots},
    {"SLS-04", "failed read sends nothing, keeps counter",
     test_sls04_failed_read_sends_nothing},
};

int main(void)
{
    return RUN_TESTS(tests);
}
