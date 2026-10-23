/* Unit tests for recording read results in the sensor health tracker
 * (src/sensor_health.c): gt_health_init and gt_health_record.
 *
 * Purpose: prove the DESIGN.md rule "after 10 consecutive failures, mark
 * offline", that a success resets the run, and that each transition is
 * reported exactly once so the log gets one sensor_offline / sensor_online
 * per outage. Pure logic: no bus, no clock. Run under make test / asan /
 * memcheck. Implements SHR-01 through SHR-07. The offline probe schedule is
 * in test_health_should_read.c.
 *
 * What these tests cannot prove: that a real unplugged sensor produces 10
 * failures. That is the driver (test_light_read.c) and the Pi. */
#include "check.h"
#include "sensor_health.h"

/* Record `read_count` results of `read_result`, one per ms starting at
 * `*clock_ms` (advanced past the last one). Returns how many of those calls
 * reported `wanted_event`. */
static int record_reads(gt_health *health, gt_status read_result, int read_count,
                        uint64_t *clock_ms, gt_health_event wanted_event)
{
    int matching_events = 0;
    for (int read = 0; read < read_count; read++) {
        if (gt_health_record(health, read_result, (*clock_ms)++) == wanted_event) {
            matching_events++;
        }
    }
    return matching_events;
}

/* ---- SHR-01: init is online with no failures ---- */
static void test_shr01_init_online(void)
{
    gt_health health;
    gt_health_init(&health);
    CHECK_EQ(health.state, GT_HEALTH_ONLINE);
    CHECK_EQ(health.consecutive_fails, 0);
}

/* ---- SHR-02: 9 consecutive failures stay online ----
 * One short of the threshold: no event, still online. */
static void test_shr02_nine_failures_stay_online(void)
{
    gt_health health;
    gt_health_init(&health);
    uint64_t clock_ms = 0;
    int below_threshold = GT_HEALTH_OFFLINE_AFTER - 1;

    CHECK_EQ(record_reads(&health, GT_ERR_IO, below_threshold, &clock_ms, GT_HEALTH_EV_NONE),
             below_threshold);
    CHECK_EQ(health.state, GT_HEALTH_ONLINE);
    CHECK_EQ(health.consecutive_fails, below_threshold);
}

/* ---- SHR-03: the 10th consecutive failure goes offline once ---- */
static void test_shr03_tenth_failure_goes_offline(void)
{
    gt_health health;
    gt_health_init(&health);
    uint64_t clock_ms = 0;
    int below_threshold = GT_HEALTH_OFFLINE_AFTER - 1;

    CHECK_EQ(record_reads(&health, GT_ERR_IO, below_threshold, &clock_ms, GT_HEALTH_EV_OFFLINE),
             0);
    CHECK_EQ(gt_health_record(&health, GT_ERR_IO, clock_ms), GT_HEALTH_EV_OFFLINE);
    CHECK_EQ(health.state, GT_HEALTH_OFFLINE);
}

/* ---- SHR-04: a success resets the failure run ----
 * 9 fail, 1 ok, 9 fail: the runs are not consecutive, so still online. */
static void test_shr04_success_resets_run(void)
{
    gt_health health;
    gt_health_init(&health);
    uint64_t clock_ms = 0;
    int below_threshold = GT_HEALTH_OFFLINE_AFTER - 1;

    record_reads(&health, GT_ERR_IO, below_threshold, &clock_ms, GT_HEALTH_EV_NONE);
    CHECK_EQ(gt_health_record(&health, GT_OK, clock_ms++), GT_HEALTH_EV_NONE);
    CHECK_EQ(health.consecutive_fails, 0);
    CHECK_EQ(record_reads(&health, GT_ERR_IO, below_threshold, &clock_ms, GT_HEALTH_EV_OFFLINE),
             0);
    CHECK_EQ(health.state, GT_HEALTH_ONLINE);
}

/* ---- SHR-05: failures while offline report nothing ----
 * The outage was already announced; failed probes must not repeat it. */
static void test_shr05_offline_failures_no_event(void)
{
    gt_health health;
    gt_health_init(&health);
    uint64_t clock_ms = 0;
    int failed_probes = 5;

    record_reads(&health, GT_ERR_IO, GT_HEALTH_OFFLINE_AFTER, &clock_ms, GT_HEALTH_EV_NONE);
    CHECK_EQ(record_reads(&health, GT_ERR_IO, failed_probes, &clock_ms, GT_HEALTH_EV_NONE),
             failed_probes);
    CHECK_EQ(health.state, GT_HEALTH_OFFLINE);
}

/* ---- SHR-06: a success while offline comes back online ----
 * Reported once, and the failure run starts over from 0. */
static void test_shr06_success_comes_online(void)
{
    gt_health health;
    gt_health_init(&health);
    uint64_t clock_ms = 0;

    record_reads(&health, GT_ERR_IO, GT_HEALTH_OFFLINE_AFTER, &clock_ms, GT_HEALTH_EV_NONE);
    CHECK_EQ(gt_health_record(&health, GT_OK, clock_ms++), GT_HEALTH_EV_ONLINE);
    CHECK_EQ(health.state, GT_HEALTH_ONLINE);
    CHECK_EQ(health.consecutive_fails, 0);
    CHECK_EQ(gt_health_record(&health, GT_OK, clock_ms++), GT_HEALTH_EV_NONE);
}

/* ---- SHR-07: a NULL tracker is rejected ---- */
static void test_shr07_null_tracker(void)
{
    gt_health_init(NULL);
    CHECK_EQ(gt_health_record(NULL, GT_ERR_IO, 0), GT_HEALTH_EV_NONE);
    CHECK_EQ(gt_health_record(NULL, GT_OK, 0), GT_HEALTH_EV_NONE);
}

static const test_case_t tests[] = {
    {"SHR-01", "init is online with no failures", test_shr01_init_online},
    {"SHR-02", "9 consecutive failures stay online", test_shr02_nine_failures_stay_online},
    {"SHR-03", "10th consecutive failure goes offline once",
     test_shr03_tenth_failure_goes_offline},
    {"SHR-04", "a success resets the failure run", test_shr04_success_resets_run},
    {"SHR-05", "failures while offline report nothing", test_shr05_offline_failures_no_event},
    {"SHR-06", "a success while offline comes back online", test_shr06_success_comes_online},
    {"SHR-07", "a NULL tracker is rejected", test_shr07_null_tracker},
};

int main(void)
{
    return RUN_TESTS(tests);
}
