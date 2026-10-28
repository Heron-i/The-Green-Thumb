/* Unit tests for the hub's last-seen timeouts (src/hub.c):
 * gt_hub_check_timeouts, and GT_HUB_EV_RESUMED from gt_hub_accept.
 *
 * Purpose: prove the DESIGN.md Hub row "Sensor goes quiet": a sensor with
 * no message for 3 s (light) or 180 s (soil) is reported once as timed
 * out, the other sensor is unaffected, and its next message reports it
 * resumed. Timers start when the hub is created, so a sensor that never
 * connects is reported too. Pure logic on a passed-in monotonic clock: no
 * sleeping. Run under make test / asan / memcheck.
 * Implements HTO-01 through HTO-06.
 *
 * What these tests cannot prove: that the rx loop calls the check often
 * enough and logs sensor_timeout (HRX does). */
#include <stdint.h>

#include "check.h"
#include "hub.h"

#define MONO_START_MS 5000ull

/* Bit gt_hub_check_timeouts sets for a sensor. */
#define LIGHT_BIT (1u << GT_SENSOR_LIGHT)
#define SOIL_BIT  (1u << GT_SENSOR_SOIL)

/* Feed `sensor_id` one DATA message (counter `counter`, on a connection
 * numbered after the sensor) at monotonic time mono_ms, and return the
 * events it brought. */
static unsigned message_at(gt_hub *hub, uint8_t sensor_id, uint32_t counter, uint64_t mono_ms)
{
    const gt_msg message = { .type = GT_MSG_DATA, .sensor_id = sensor_id,
                             .counter = counter, .seq = 0, .value = 100 };
    const gt_hub_clock now = { .wall_us = 1700000000000000ull + mono_ms * 1000u,
                               .mono_ms = mono_ms };
    gt_hub_result result;
    CHECK_EQ(gt_hub_accept(hub, sensor_id, &message, &now, &result), GT_OK);
    return result.events;
}

/* ---- HTO-01: light times out after exactly 3 s ----
 * Soil is kept fresh, so only the light bit can be set. */
static void test_hto01_light_after_3s(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    uint64_t light_seen = MONO_START_MS + 100;
    message_at(hub, GT_SENSOR_LIGHT, 0, light_seen);
    message_at(hub, GT_SENSOR_SOIL, 0, light_seen);

    CHECK_EQ(gt_hub_check_timeouts(hub, light_seen + GT_HUB_LIGHT_TIMEOUT_MS - 1), 0);
    CHECK_EQ(gt_hub_check_timeouts(hub, light_seen + GT_HUB_LIGHT_TIMEOUT_MS), LIGHT_BIT);
    gt_hub_destroy(hub);
}

/* ---- HTO-02: soil times out after exactly 180 s ----
 * Light keeps sending once a second the whole time, so it never times
 * out; only the soil bit can appear, and only at 180 s. */
static void test_hto02_soil_after_180s(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    uint64_t soil_seen = MONO_START_MS;
    message_at(hub, GT_SENSOR_SOIL, 0, soil_seen);

    uint32_t light_counter = 0;
    /* Invariant: light has sent once a second up to `elapsed_ms`, so the
     * only sensor that can time out is soil, and only at 180 s. */
    for (uint64_t elapsed_ms = 1000; elapsed_ms < GT_HUB_SOIL_TIMEOUT_MS; elapsed_ms += 1000) {
        message_at(hub, GT_SENSOR_LIGHT, light_counter++, soil_seen + elapsed_ms);
        CHECK_EQ(gt_hub_check_timeouts(hub, soil_seen + elapsed_ms), 0);
    }
    message_at(hub, GT_SENSOR_LIGHT, light_counter, soil_seen + GT_HUB_SOIL_TIMEOUT_MS);
    CHECK_EQ(gt_hub_check_timeouts(hub, soil_seen + GT_HUB_SOIL_TIMEOUT_MS), SOIL_BIT);
    gt_hub_destroy(hub);
}

/* ---- HTO-03: a quiet sensor is reported once per spell ----
 * The rx loop checks every 100 ms; a light sensor unplugged for a minute
 * must give one sensor_timeout, not 570. */
static void test_hto03_once_per_spell(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    uint64_t light_seen = MONO_START_MS;
    message_at(hub, GT_SENSOR_LIGHT, 0, light_seen);
    message_at(hub, GT_SENSOR_SOIL, 0, light_seen);

    CHECK_EQ(gt_hub_check_timeouts(hub, light_seen + GT_HUB_LIGHT_TIMEOUT_MS), LIGHT_BIT);
    CHECK_EQ(gt_hub_check_timeouts(hub, light_seen + GT_HUB_LIGHT_TIMEOUT_MS + 100), 0);
    CHECK_EQ(gt_hub_check_timeouts(hub, light_seen + 60000), 0);
    gt_hub_destroy(hub);
}

/* ---- HTO-04: the next message resumes, then a new spell ----
 * After the timeout, light's next message brings RESUMED (once: the one
 * after it is quiet). Going quiet again 3 s later is a new spell and is
 * reported again. */
static void test_hto04_resumed_then_new_spell(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    uint64_t light_seen = MONO_START_MS;
    message_at(hub, GT_SENSOR_LIGHT, 0, light_seen);
    message_at(hub, GT_SENSOR_SOIL, 0, light_seen);
    uint64_t timed_out_at = light_seen + GT_HUB_LIGHT_TIMEOUT_MS;
    CHECK_EQ(gt_hub_check_timeouts(hub, timed_out_at), LIGHT_BIT);

    uint64_t back_at = timed_out_at + 2000;
    CHECK_EQ(message_at(hub, GT_SENSOR_LIGHT, 1, back_at), GT_HUB_EV_RESUMED);
    CHECK_EQ(message_at(hub, GT_SENSOR_LIGHT, 2, back_at + 1000), 0);
    CHECK_EQ(gt_hub_check_timeouts(hub, back_at + 1000 + GT_HUB_LIGHT_TIMEOUT_MS - 1), 0);
    CHECK_EQ(gt_hub_check_timeouts(hub, back_at + 1000 + GT_HUB_LIGHT_TIMEOUT_MS), LIGHT_BIT);
    gt_hub_destroy(hub);
}

/* ---- HTO-05: a never-connected sensor times out from start ----
 * Neither sensor ever sends. Light is reported 3 s after the hub was
 * created and soil 180 s after; each once. A sensor that is never plugged
 * in is a fault the log must show. */
static void test_hto05_never_connected(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);

    CHECK_EQ(gt_hub_check_timeouts(hub, MONO_START_MS + GT_HUB_LIGHT_TIMEOUT_MS - 1), 0);
    CHECK_EQ(gt_hub_check_timeouts(hub, MONO_START_MS + GT_HUB_LIGHT_TIMEOUT_MS), LIGHT_BIT);
    CHECK_EQ(gt_hub_check_timeouts(hub, MONO_START_MS + GT_HUB_SOIL_TIMEOUT_MS - 1), 0);
    CHECK_EQ(gt_hub_check_timeouts(hub, MONO_START_MS + GT_HUB_SOIL_TIMEOUT_MS), SOIL_BIT);
    CHECK_EQ(gt_hub_check_timeouts(hub, MONO_START_MS + 2 * GT_HUB_SOIL_TIMEOUT_MS), 0);
    gt_hub_destroy(hub);
}

/* ---- HTO-06: NULL hub or an earlier now_ms: no timeout ----
 * now_ms - last_seen_ms is unsigned, so a now_ms before the last-seen
 * time would wrap to a huge number and look like a timeout; it must not. */
static void test_hto06_null_and_earlier_now(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);

    CHECK_EQ(gt_hub_check_timeouts(NULL, MONO_START_MS + GT_HUB_SOIL_TIMEOUT_MS), 0);
    CHECK_EQ(gt_hub_check_timeouts(hub, MONO_START_MS - 1), 0);
    CHECK_EQ(gt_hub_check_timeouts(hub, 0), 0);
    gt_hub_destroy(hub);
}

static const test_case_t tests[] = {
    {"HTO-01", "light times out after exactly 3 s", test_hto01_light_after_3s},
    {"HTO-02", "soil times out after exactly 180 s", test_hto02_soil_after_180s},
    {"HTO-03", "a quiet sensor is reported once per spell", test_hto03_once_per_spell},
    {"HTO-04", "next message resumes, then a new spell", test_hto04_resumed_then_new_spell},
    {"HTO-05", "never-connected sensor times out from start", test_hto05_never_connected},
    {"HTO-06", "NULL hub or an earlier now_ms: no timeout", test_hto06_null_and_earlier_now},
};

int main(void)
{
    return RUN_TESTS(tests);
}
