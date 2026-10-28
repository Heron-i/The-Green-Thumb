/* Unit tests for the hub's per-message logic (src/hub.c): gt_hub_accept.
 *
 * Purpose: prove what the hub does with each decoded sensor message before
 * it is queued for storage (DESIGN.md Hub rows; Sensor-Pipeline.md Hub
 * contract and Hub steps). The record keeps the receive time and leaves
 * seq at 0 for the sender (PF-01); out-of-range values, duplicates,
 * counter jumps, new connections and sensor restarts are each reported
 * with their own event. Pure logic with the clocks passed in: no sockets,
 * no waiting. Run under make test / asan / memcheck.
 * Implements HAC-01 through HAC-13.
 *
 * What these tests cannot prove: that the rx loop logs the events and
 * closes the older connection (HRX does), or that the sender stamps seq
 * (HSS does). Timeouts and sensor_resumed are HTO's. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "hub.h"

/* Times used throughout: wall clock in us since the epoch, monotonic in ms. */
#define WALL_START_US 1700000000000000ull
#define MONO_START_MS 5000ull

/* A message as gt_msg_decode would hand it over (seq is always 0 from a
 * sensor). */
static gt_msg make_msg(uint8_t type, uint8_t sensor_id, uint32_t counter, uint32_t value)
{
    gt_msg message = { .type = type, .sensor_id = sensor_id, .counter = counter,
                       .seq = 0, .value = value };
    return message;
}

/* Both clocks `step_ms` ms after the start, moving together. */
static gt_hub_clock clock_at(uint64_t step_ms)
{
    gt_hub_clock now = { .wall_us = WALL_START_US + step_ms * 1000u,
                         .mono_ms = MONO_START_MS + step_ms };
    return now;
}

/* ---- HAC-01: first DATA becomes an unstamped record ----
 * The record carries the message's type, sensor and value, the wall-clock
 * receive time, and seq 0 for the sender to stamp. A sensor's very first
 * message is also its first on this connection: NEW_CLIENT only. */
static void test_hac01_first_data_record(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    CHECK(hub != NULL);
    const gt_msg message = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 0, 1234);
    const gt_hub_clock now = clock_at(0);
    gt_hub_result result;

    CHECK_EQ(gt_hub_accept(hub, 1, &message, &now, &result), GT_OK);
    CHECK_EQ(result.has_record, 1);
    CHECK_EQ(result.record.type, GT_REC_DATA);
    CHECK_EQ(result.record.sensor_id, GT_SENSOR_LIGHT);
    CHECK_EQ(result.record.value, 1234);
    CHECK_EQ(result.record.seq, 0);
    CHECK(result.record.timestamp_us == WALL_START_US);
    CHECK_EQ(result.events, GT_HUB_EV_NEW_CLIENT);
    CHECK(result.last_seen_us == 0);
    gt_hub_destroy(hub);
}

/* ---- HAC-02: the next counter on the same connection is quiet ----
 * Each record gets its own receive time, and last_seen_us reports the
 * previous message's. */
static void test_hac02_next_counter_quiet(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_msg first = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 7, 100);
    const gt_msg second = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 8, 101);
    const gt_hub_clock first_at = clock_at(0);
    const gt_hub_clock second_at = clock_at(1000);
    gt_hub_result result;

    CHECK_EQ(gt_hub_accept(hub, 1, &first, &first_at, &result), GT_OK);
    CHECK_EQ(gt_hub_accept(hub, 1, &second, &second_at, &result), GT_OK);
    CHECK_EQ(result.has_record, 1);
    CHECK_EQ(result.events, 0);
    CHECK_EQ(result.record.value, 101);
    CHECK(result.record.timestamp_us == second_at.wall_us);
    CHECK(result.last_seen_us == first_at.wall_us);
    gt_hub_destroy(hub);
}

/* ---- HAC-03: light above 65535 is reading_invalid ----
 * 65535 (direct sun, saturated) is real data and is kept; 65536 can't come
 * from a 16-bit sensor, so it is rejected and nothing is queued. */
static void test_hac03_light_range(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_msg at_max = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 0, GT_HUB_LIGHT_RAW_MAX);
    const gt_msg over_max = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 1, GT_HUB_LIGHT_RAW_MAX + 1);
    const gt_hub_clock now = clock_at(0);
    gt_hub_result result;

    CHECK_EQ(gt_hub_accept(hub, 1, &at_max, &now, &result), GT_OK);
    CHECK_EQ(result.has_record, 1);
    CHECK_EQ(gt_hub_accept(hub, 1, &over_max, &now, &result), GT_ERR_CORRUPT);
    CHECK_EQ(result.has_record, 0);
    CHECK_EQ(result.events, GT_HUB_EV_READING_INVALID);
    gt_hub_destroy(hub);
}

/* ---- HAC-04: soil 0xFFFF is reading_invalid ----
 * 0xFFFF is the seesaw's error code; 0xFFFE, and a low open-air count
 * like 0, are stored (Sensor-Pipeline.md Decisions). */
static void test_hac04_soil_range(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_msg open_air = make_msg(GT_MSG_DATA, GT_SENSOR_SOIL, 0, 0);
    const gt_msg at_max = make_msg(GT_MSG_DATA, GT_SENSOR_SOIL, 1, GT_HUB_SOIL_RAW_MAX);
    const gt_msg error_code = make_msg(GT_MSG_DATA, GT_SENSOR_SOIL, 2, 0xFFFFu);
    const gt_hub_clock now = clock_at(0);
    gt_hub_result result;

    CHECK_EQ(gt_hub_accept(hub, 1, &open_air, &now, &result), GT_OK);
    CHECK_EQ(gt_hub_accept(hub, 1, &at_max, &now, &result), GT_OK);
    CHECK_EQ(result.has_record, 1);
    CHECK_EQ(gt_hub_accept(hub, 1, &error_code, &now, &result), GT_ERR_CORRUPT);
    CHECK_EQ(result.has_record, 0);
    CHECK_EQ(result.events, GT_HUB_EV_READING_INVALID);
    gt_hub_destroy(hub);
}

/* ---- HAC-05: an old counter on the same conn is a duplicate ----
 * After 10 and 11 the hub expects 12; a resent 11 is dropped (nothing to
 * queue) and reported with the counter the hub expected. A duplicate is
 * not an error to the caller. */
static void test_hac05_duplicate_dropped(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_msg ten = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 10, 500);
    const gt_msg eleven = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 11, 501);
    const gt_hub_clock now = clock_at(0);
    gt_hub_result result;

    CHECK_EQ(gt_hub_accept(hub, 1, &ten, &now, &result), GT_OK);
    CHECK_EQ(gt_hub_accept(hub, 1, &eleven, &now, &result), GT_OK);
    CHECK_EQ(gt_hub_accept(hub, 1, &eleven, &now, &result), GT_OK);
    CHECK_EQ(result.has_record, 0);
    CHECK_EQ(result.events, GT_HUB_EV_DUPLICATE);
    CHECK_EQ(result.expected_counter, 12);
    gt_hub_destroy(hub);
}

/* ---- HAC-06: a counter jump is queued and reported ----
 * 42 then 50: messages 43-49 were lost in transit (the kernel took them
 * but the hub never read them). 50 is still real data, so it is queued;
 * the jump is logged with the counter the hub expected, and the next one
 * after 50 is quiet again. */
static void test_hac06_counter_jump(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_msg before = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 42, 1);
    const gt_msg jumped = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 50, 2);
    const gt_msg after = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 51, 3);
    const gt_hub_clock now = clock_at(0);
    gt_hub_result result;

    CHECK_EQ(gt_hub_accept(hub, 1, &before, &now, &result), GT_OK);
    CHECK_EQ(gt_hub_accept(hub, 1, &jumped, &now, &result), GT_OK);
    CHECK_EQ(result.has_record, 1);
    CHECK_EQ(result.record.value, 2);
    CHECK_EQ(result.events, GT_HUB_EV_SEQ_GAP);
    CHECK_EQ(result.expected_counter, 43);
    CHECK_EQ(gt_hub_accept(hub, 1, &after, &now, &result), GT_OK);
    CHECK_EQ(result.events, 0);
    gt_hub_destroy(hub);
}

/* ---- HAC-07: a GAP message is stamped with the last DATA time ----
 * The sensor refused counters 65-90 (its ring overflowed while the hub
 * was down), then sent GAP{first=65, count=26}. The GAP record keeps the
 * count, and its time is when the last DATA (64) arrived: the earliest
 * the gap could have started (as PF-07 does for storage). The GAP covers
 * up to 90, so DATA 91 is quiet. */
static void test_hac07_gap_message_time(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_msg last_data = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 64, 10);
    const gt_msg gap = make_msg(GT_MSG_GAP, GT_SENSOR_LIGHT, 65, 26);
    const gt_msg next_data = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 91, 11);
    const gt_hub_clock last_data_at = clock_at(0);
    const gt_hub_clock gap_at = clock_at(90000);
    gt_hub_result result;

    CHECK_EQ(gt_hub_accept(hub, 1, &last_data, &last_data_at, &result), GT_OK);
    CHECK_EQ(gt_hub_accept(hub, 1, &gap, &gap_at, &result), GT_OK);
    CHECK_EQ(result.has_record, 1);
    CHECK_EQ(result.record.type, GT_REC_GAP);
    CHECK_EQ(result.record.sensor_id, GT_SENSOR_LIGHT);
    CHECK_EQ(result.record.value, 26);
    CHECK_EQ(result.record.seq, 0);
    CHECK(result.record.timestamp_us == last_data_at.wall_us);
    CHECK_EQ(result.events, 0);
    CHECK_EQ(gt_hub_accept(hub, 1, &next_data, &gap_at, &result), GT_OK);
    CHECK_EQ(result.events, 0);
    gt_hub_destroy(hub);
}

/* ---- HAC-08: a GAP with no earlier DATA is stamped now ----
 * A freshly started hub can get a GAP as the light sensor's first message
 * (the sensor buffered while the old hub was gone). There is no light
 * DATA time to use, so the receive time is used; a soil DATA that came
 * earlier must not lend its time. */
static void test_hac08_gap_without_data(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_msg soil_data = make_msg(GT_MSG_DATA, GT_SENSOR_SOIL, 0, 900);
    const gt_msg light_gap = make_msg(GT_MSG_GAP, GT_SENSOR_LIGHT, 0, 5);
    const gt_hub_clock soil_at = clock_at(0);
    const gt_hub_clock gap_at = clock_at(2000);
    gt_hub_result result;

    CHECK_EQ(gt_hub_accept(hub, 1, &soil_data, &soil_at, &result), GT_OK);
    CHECK_EQ(gt_hub_accept(hub, 2, &light_gap, &gap_at, &result), GT_OK);
    CHECK_EQ(result.has_record, 1);
    CHECK_EQ(result.record.type, GT_REC_GAP);
    CHECK(result.record.timestamp_us == gap_at.wall_us);
    CHECK_EQ(result.events, GT_HUB_EV_NEW_CLIENT);
    gt_hub_destroy(hub);
}

/* ---- HAC-09: a reconnect that carries on is NEW_CLIENT only ----
 * The sensor's client lost its connection (say the hub closed it after a
 * bad packet) and reconnected with its counter intact: 20 on conn 1, then
 * 21 on conn 2. Same process, no loss, so no restart and no gap. */
static void test_hac09_reconnect_continues(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_msg on_old = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 20, 1);
    const gt_msg on_new = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 21, 2);
    const gt_hub_clock now = clock_at(0);
    gt_hub_result result;

    CHECK_EQ(gt_hub_accept(hub, 1, &on_old, &now, &result), GT_OK);
    CHECK_EQ(gt_hub_accept(hub, 2, &on_new, &now, &result), GT_OK);
    CHECK_EQ(result.has_record, 1);
    CHECK_EQ(result.events, GT_HUB_EV_NEW_CLIENT);
    gt_hub_destroy(hub);
}

/* ---- HAC-10: a counter reset on a new conn is a restart ----
 * The light process died after counter 300 and the supervisor restarted
 * it: its first message, on a new connection, is counter 0. That reading
 * is real and is queued (not a duplicate), and the restart is reported
 * with the previous message's receive time; the new one is the record's
 * own time. No GAP record is made (Sensor-Pipeline.md Decisions). */
static void test_hac10_counter_reset_restart(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_msg before_crash = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 300, 1);
    const gt_msg after_restart = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 0, 2);
    const gt_hub_clock crash_at = clock_at(0);
    const gt_hub_clock restart_at = clock_at(4000);
    gt_hub_result result;

    CHECK_EQ(gt_hub_accept(hub, 1, &before_crash, &crash_at, &result), GT_OK);
    CHECK_EQ(gt_hub_accept(hub, 2, &after_restart, &restart_at, &result), GT_OK);
    CHECK_EQ(result.has_record, 1);
    CHECK_EQ(result.record.type, GT_REC_DATA);
    CHECK(result.record.timestamp_us == restart_at.wall_us);
    CHECK_EQ(result.events, GT_HUB_EV_NEW_CLIENT | GT_HUB_EV_SENSOR_RESTART);
    CHECK(result.last_seen_us == crash_at.wall_us);
    gt_hub_destroy(hub);
}

/* ---- HAC-11: an invalid reading still moves the counter on ----
 * Counter 5 is out of range and rejected, but the sensor did send it, so
 * the hub expects 6 next: 6 is quiet, not a sensor_seq_gap. */
static void test_hac11_invalid_moves_counter(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_msg good = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 4, 10);
    const gt_msg invalid = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 5, GT_HUB_LIGHT_RAW_MAX + 1);
    const gt_msg next = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 6, 12);
    const gt_hub_clock now = clock_at(0);
    gt_hub_result result;

    CHECK_EQ(gt_hub_accept(hub, 1, &good, &now, &result), GT_OK);
    CHECK_EQ(gt_hub_accept(hub, 1, &invalid, &now, &result), GT_ERR_CORRUPT);
    CHECK_EQ(gt_hub_accept(hub, 1, &next, &now, &result), GT_OK);
    CHECK_EQ(result.has_record, 1);
    CHECK_EQ(result.events, 0);
    gt_hub_destroy(hub);
}

/* ---- HAC-12: light and soil are tracked separately ----
 * Light on conn 1 sends 0, 1, 2 and soil on conn 2 sends 0, 1, all
 * interleaved. Each stream is contiguous on its own, so after each
 * sensor's first message (NEW_CLIENT) there is no event at all: soil's 0
 * is neither a duplicate of light's nor a restart. */
static void test_hac12_sensors_separate(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_hub_clock now = clock_at(0);
    gt_hub_result result;
    /* Arrival order: L0 S0 L1 S1 L2. */
    const gt_msg arrivals[] = {
        make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 0, 1), make_msg(GT_MSG_DATA, GT_SENSOR_SOIL, 0, 800),
        make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 1, 2), make_msg(GT_MSG_DATA, GT_SENSOR_SOIL, 1, 801),
        make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 2, 3),
    };
    const uint32_t conn_of[] = { 1, 2, 1, 2, 1 };
    const unsigned expected_events[] = { GT_HUB_EV_NEW_CLIENT, GT_HUB_EV_NEW_CLIENT, 0, 0, 0 };

    /* Invariant: arrivals[0..arrival_index-1] were each accepted with
     * exactly the event they should bring. */
    for (size_t arrival_index = 0; arrival_index < sizeof arrivals / sizeof arrivals[0];
         arrival_index++) {
        CHECK_EQ(gt_hub_accept(hub, conn_of[arrival_index], &arrivals[arrival_index], &now,
                               &result), GT_OK);
        CHECK_EQ(result.has_record, 1);
        CHECK_EQ(result.events, expected_events[arrival_index]);
    }
    gt_hub_destroy(hub);
}

/* ---- HAC-13: bad arguments are rejected, result untouched ----
 * NULL pointers, and a type or sensor_id that gt_msg_decode would never
 * let through (sensor 0 "all" and 3 unknown; type 3 unknown), are
 * GT_ERR_INVAL. The result is pre-filled with a pattern and compared with
 * memcmp, because copying a struct with `=` isn't guaranteed to copy its
 * padding bytes. gt_hub_destroy(NULL) is safe. */
static void test_hac13_bad_args(void)
{
    gt_hub *hub = gt_hub_create(MONO_START_MS);
    const gt_msg good = make_msg(GT_MSG_DATA, GT_SENSOR_LIGHT, 0, 1);
    const gt_msg bad_messages[] = {
        make_msg(GT_MSG_DATA, GT_SENSOR_ALL, 0, 1),
        make_msg(GT_MSG_DATA, 3, 0, 1),
        make_msg(3, GT_SENSOR_LIGHT, 0, 1),
    };
    const gt_hub_clock now = clock_at(0);
    gt_hub_result result;
    gt_hub_result pattern;
    memset(&pattern, 0x5A, sizeof pattern);
    memcpy(&result, &pattern, sizeof result);

    CHECK_EQ(gt_hub_accept(NULL, 1, &good, &now, &result), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_accept(hub, 1, NULL, &now, &result), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_accept(hub, 1, &good, NULL, &result), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_accept(hub, 1, &good, &now, NULL), GT_ERR_INVAL);
    /* Invariant: bad_messages[0..bad_index-1] were each rejected. */
    for (size_t bad_index = 0; bad_index < sizeof bad_messages / sizeof bad_messages[0];
         bad_index++) {
        CHECK_EQ(gt_hub_accept(hub, 1, &bad_messages[bad_index], &now, &result), GT_ERR_INVAL);
    }
    CHECK(memcmp(&result, &pattern, sizeof result) == 0);
    gt_hub_destroy(hub);
    gt_hub_destroy(NULL);
}

static const test_case_t tests[] = {
    {"HAC-01", "first DATA becomes an unstamped record", test_hac01_first_data_record},
    {"HAC-02", "next counter on the same conn is quiet", test_hac02_next_counter_quiet},
    {"HAC-03", "light above 65535 is reading_invalid", test_hac03_light_range},
    {"HAC-04", "soil 0xFFFF is reading_invalid", test_hac04_soil_range},
    {"HAC-05", "old counter on the same conn is a duplicate", test_hac05_duplicate_dropped},
    {"HAC-06", "a counter jump is queued and reported", test_hac06_counter_jump},
    {"HAC-07", "GAP message stamped with last DATA time", test_hac07_gap_message_time},
    {"HAC-08", "a GAP with no earlier DATA is stamped now", test_hac08_gap_without_data},
    {"HAC-09", "reconnect that carries on is NEW_CLIENT", test_hac09_reconnect_continues},
    {"HAC-10", "a counter reset on a new conn is a restart", test_hac10_counter_reset_restart},
    {"HAC-11", "an invalid reading still moves the counter", test_hac11_invalid_moves_counter},
    {"HAC-12", "light and soil are tracked separately", test_hac12_sensors_separate},
    {"HAC-13", "bad arguments are rejected, result untouched", test_hac13_bad_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
