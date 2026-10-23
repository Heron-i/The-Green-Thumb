/* Unit tests for the sensor health log event names (src/sensor_health.c):
 * gt_health_event_name and GT_HEALTH_I2C_READ_ERROR.
 *
 * Purpose: prove each name matches DESIGN.md's failure-mode table exactly
 * (Sensor (I2C) row: i2c_read_error, sensor_offline, sensor_online), and
 * that "no transition" maps to nothing, so the caller logs only real
 * changes. Run under make test / asan / memcheck. Implements SHE-01 through
 * SHE-05. Producing the transitions is in test_health_record.c.
 *
 * What these tests cannot prove: that the light process actually logs the
 * names. That arrives with src/light_sensor_main.c. */
#include <string.h>

#include "check.h"
#include "sensor_health.h"

/* ---- SHE-01: OFFLINE is named sensor_offline ---- */
static void test_she01_offline_name(void)
{
    const char *event_name = gt_health_event_name(GT_HEALTH_EV_OFFLINE);
    CHECK(event_name != NULL && strcmp(event_name, "sensor_offline") == 0);
}

/* ---- SHE-02: ONLINE is named sensor_online ---- */
static void test_she02_online_name(void)
{
    const char *event_name = gt_health_event_name(GT_HEALTH_EV_ONLINE);
    CHECK(event_name != NULL && strcmp(event_name, "sensor_online") == 0);
}

/* ---- SHE-03: NONE has no name ----
 * No transition, so nothing to log. */
static void test_she03_none_has_no_name(void)
{
    CHECK(gt_health_event_name(GT_HEALTH_EV_NONE) == NULL);
}

/* ---- SHE-04: an unknown value has no name ----
 * An enum is just an int in C, so a stray value can reach the function;
 * it must return NULL rather than a wrong name. */
static void test_she04_unknown_has_no_name(void)
{
    CHECK(gt_health_event_name((gt_health_event)99) == NULL);
}

/* ---- SHE-05: the read-error name is i2c_read_error ----
 * Pinned like the I2C address (LSO-01): a typo here would split one event
 * into two names in the logs. */
static void test_she05_read_error_name(void)
{
    CHECK(strcmp(GT_HEALTH_I2C_READ_ERROR, "i2c_read_error") == 0);
}

static const test_case_t tests[] = {
    {"SHE-01", "OFFLINE is named sensor_offline", test_she01_offline_name},
    {"SHE-02", "ONLINE is named sensor_online", test_she02_online_name},
    {"SHE-03", "NONE has no name", test_she03_none_has_no_name},
    {"SHE-04", "an unknown value has no name", test_she04_unknown_has_no_name},
    {"SHE-05", "the read-error name is i2c_read_error", test_she05_read_error_name},
};

int main(void)
{
    return RUN_TESTS(tests);
}
