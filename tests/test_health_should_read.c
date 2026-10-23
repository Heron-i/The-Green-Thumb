/* Unit tests for the sensor health tracker's probe schedule
 * (src/sensor_health.c): gt_health_should_read.
 *
 * Purpose: prove the DESIGN.md rule "while offline, retry every 5 s": an
 * online sensor is read every time it is due, an offline one only once per
 * GT_HEALTH_RETRY_MS, measured from the last failed probe. Pure logic: time
 * is passed in. Run under make test / asan / memcheck. Implements SHS-01
 * through SHS-05. Recording results is in test_health_record.c.
 *
 * What these tests cannot prove: when reads are due at all (1 Hz). That is
 * the sampler's schedule. */
#include "check.h"
#include "sensor_health.h"

/* Start `health` fresh and take it offline: all 10 failures land at
 * `offline_at_ms`. */
static void go_offline_at(gt_health *health, uint64_t offline_at_ms)
{
    gt_health_init(health);
    for (int failure = 0; failure < GT_HEALTH_OFFLINE_AFTER; failure++) {
        gt_health_record(health, GT_ERR_IO, offline_at_ms);
    }
}

/* ---- SHS-01: an online sensor is always read ---- */
static void test_shs01_online_always_reads(void)
{
    gt_health health;
    gt_health_init(&health);
    CHECK(gt_health_should_read(&health, 0));
    CHECK(gt_health_should_read(&health, 1));
    CHECK(gt_health_should_read(&health, UINT64_MAX));
}

/* ---- SHS-02: offline waits 5 s before the first probe ---- */
static void test_shs02_offline_waits_retry_period(void)
{
    gt_health health;
    uint64_t offline_at_ms = 1000;
    go_offline_at(&health, offline_at_ms);

    CHECK(!gt_health_should_read(&health, offline_at_ms));
    CHECK(!gt_health_should_read(&health, offline_at_ms + GT_HEALTH_RETRY_MS - 1));
    CHECK(gt_health_should_read(&health, offline_at_ms + GT_HEALTH_RETRY_MS));
}

/* ---- SHS-03: a failed probe reschedules from the probe time ----
 * Probing late (offline + 7000) pushes the next probe to offline + 12000,
 * not offline + 10000, so the device is never probed faster than every 5 s. */
static void test_shs03_failed_probe_reschedules(void)
{
    gt_health health;
    uint64_t offline_at_ms = 1000;
    go_offline_at(&health, offline_at_ms);

    uint64_t late_probe_ms = offline_at_ms + 7000;
    CHECK(gt_health_should_read(&health, late_probe_ms));
    CHECK_EQ(gt_health_record(&health, GT_ERR_IO, late_probe_ms), GT_HEALTH_EV_NONE);
    CHECK(!gt_health_should_read(&health, late_probe_ms + GT_HEALTH_RETRY_MS - 1));
    CHECK(gt_health_should_read(&health, late_probe_ms + GT_HEALTH_RETRY_MS));
}

/* ---- SHS-04: back online is read immediately ---- */
static void test_shs04_online_again_reads_now(void)
{
    gt_health health;
    uint64_t offline_at_ms = 1000;
    go_offline_at(&health, offline_at_ms);

    uint64_t probe_ms = offline_at_ms + GT_HEALTH_RETRY_MS;
    CHECK_EQ(gt_health_record(&health, GT_OK, probe_ms), GT_HEALTH_EV_ONLINE);
    CHECK(gt_health_should_read(&health, probe_ms + 1));
}

/* ---- SHS-05: a NULL tracker is never read ---- */
static void test_shs05_null_tracker(void)
{
    CHECK(!gt_health_should_read(NULL, 0));
}

static const test_case_t tests[] = {
    {"SHS-01", "an online sensor is always read", test_shs01_online_always_reads},
    {"SHS-02", "offline waits 5 s before the first probe", test_shs02_offline_waits_retry_period},
    {"SHS-03", "failed probe reschedules from probe time", test_shs03_failed_probe_reschedules},
    {"SHS-04", "back online is read immediately", test_shs04_online_again_reads_now},
    {"SHS-05", "a NULL tracker is never read", test_shs05_null_tracker},
};

int main(void)
{
    return RUN_TESTS(tests);
}
