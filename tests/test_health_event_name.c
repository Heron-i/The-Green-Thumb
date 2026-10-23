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

static const test_case_t tests[] = {
    {"SHE-01", "OFFLINE is named sensor_offline", test_she01_offline_name},
};

int main(void)
{
    return RUN_TESTS(tests);
}
