/* The hub's per-message logic (see hub.h).
 *
 * Purpose: decide what happens to each decoded sensor message before it is
 * queued for storage. Range-check the value (reading_invalid), compare the
 * sensor's counter with the one expected (ipc_duplicate, sensor_seq_gap),
 * notice a new connection and a restarted sensor, and build the record:
 * type, sensor, value and receive time, with seq left at 0 for the hop-2
 * sender to stamp at send time (PF-01).
 *
 * Pure logic, like sensor_health.c: both clocks are passed in, nothing is
 * logged, no system calls. The hub keeps one small tracker per sensor, so
 * light and soil never affect each other's counters. Rules: DESIGN.md
 * failure-mode table, Hub rows; Sensor-Pipeline.md, Hub steps. */
#include "hub.h"

#include <stdlib.h>
#include <string.h>

/* What the hub remembers about one sensor between messages. */
typedef struct {
    int      seen;              /* 0 until the first message from this sensor */
    uint32_t owner_conn;        /* conn_id that sent its latest message */
    uint32_t expected_counter;  /* the counter its next message should carry */
    int      has_data;          /* 1 once a DATA message has arrived */
    uint64_t last_data_us;      /* wall-clock receive time of its latest DATA */
    uint64_t last_seen_us;      /* wall-clock receive time of its latest message */
    uint64_t last_seen_ms;      /* the same moment on the monotonic clock (timeouts) */
    int      stale;             /* 1 while timed out (set by gt_hub_check_timeouts) */
} sensor_track;

/* One tracker per sensor, indexed by sensor_id (GT_SENSOR_LIGHT = 1,
 * GT_SENSOR_SOIL = 2); index 0 ("all") is never used for a message. */
struct gt_hub {
    sensor_track sensors[GT_SENSOR_SOIL + 1];
};

/* calloc zeroes the struct, so every sensor starts unseen and not stale.
 * Only the timeout timers need a value: they start now, so a sensor that
 * never connects still times out once. */
gt_hub *gt_hub_create(uint64_t now_ms)
{
    gt_hub *hub = calloc(1, sizeof *hub);
    if (hub == NULL) {
        return NULL;
    }
    /* Invariant: sensors[0..sensor_index-1] have their timer started. */
    for (size_t sensor_index = 0; sensor_index <= GT_SENSOR_SOIL; sensor_index++) {
        hub->sensors[sensor_index].last_seen_ms = now_ms;
    }
    return hub;
}

/* 1 if the message has a type and sensor_id gt_msg_decode would let
 * through: DATA or GAP, from LIGHT or SOIL. The rx loop only hands over
 * decoded frames, so this guards direct callers, and it keeps the
 * sensor_id safe to use as an index into hub->sensors. */
static int message_is_valid(const gt_msg *message)
{
    int type_ok = message->type == GT_MSG_DATA || message->type == GT_MSG_GAP;
    int sensor_ok = message->sensor_id == GT_SENSOR_LIGHT || message->sensor_id == GT_SENSOR_SOIL;
    return type_ok && sensor_ok;
}

/* 1 if a DATA message's raw count is one its sensor can produce (DESIGN.md
 * Hub row "Out-of-range reading"): light up to GT_HUB_LIGHT_RAW_MAX, soil
 * up to GT_HUB_SOIL_RAW_MAX (only the seesaw's 0xFFFF error code is out).
 * A GAP's value is a count of refused readings, not a reading, so a GAP
 * is always in range. */
static int value_in_range(const gt_msg *message)
{
    if (message->type == GT_MSG_GAP) {
        return 1;
    }
    uint32_t raw_max = message->sensor_id == GT_SENSOR_LIGHT ? GT_HUB_LIGHT_RAW_MAX
                                                             : GT_HUB_SOIL_RAW_MAX;
    return message->value <= raw_max;
}

/* The events a message's counter brings, judged against what the hub
 * remembers about its sensor (`track`), before anything is updated:
 *   - first message ever from this sensor:     NEW_CLIENT
 *   - first on a new connection (conn_id):     NEW_CLIENT, plus
 *       SENSOR_RESTART if the counter went back (the process restarted
 *       and started again at 0), or SEQ_GAP if it jumped ahead
 *   - same connection, counter went back:      DUPLICATE (a resend)
 *   - same connection, counter jumped ahead:   SEQ_GAP
 * A counter equal to the expected one brings nothing. */
static unsigned counter_events(const sensor_track *track, uint32_t conn_id, uint32_t counter)
{
    if (!track->seen) {
        return GT_HUB_EV_NEW_CLIENT;
    }
    int new_conn = conn_id != track->owner_conn;
    unsigned events = new_conn ? GT_HUB_EV_NEW_CLIENT : 0u;
    if (counter < track->expected_counter) {
        events |= new_conn ? GT_HUB_EV_SENSOR_RESTART : GT_HUB_EV_DUPLICATE;
    } else if (counter > track->expected_counter) {
        events |= GT_HUB_EV_SEQ_GAP;
    }
    return events;
}

/* Remember a message once it has been judged: who sent it, when it
 * arrived (both clocks), and that the sensor is no longer stale. When
 * move_counter is 1 the expected counter moves past the message: +1 for
 * DATA, or past the whole refused range counter..counter+value-1 for a
 * GAP. A duplicate passes 0, because its counter is behind the expected
 * one and must not pull it back. */
static void note_message(sensor_track *track, uint32_t conn_id, const gt_msg *message,
                         const gt_hub_clock *now, int move_counter)
{
    if (move_counter) {
        uint32_t span = message->type == GT_MSG_GAP ? message->value : 1u;
        track->expected_counter = message->counter + span;
    }
    if (message->type == GT_MSG_DATA) {
        track->has_data = 1;
        track->last_data_us = now->wall_us;
    }
    track->seen = 1;
    track->owner_conn = conn_id;
    track->last_seen_us = now->wall_us;
    track->last_seen_ms = now->mono_ms;
    track->stale = 0;
}

/* Judge one message, then remember it (see hub.h for the full contract).
 * The decision is built in a local and copied out at the end, so *result
 * is untouched on GT_ERR_INVAL. The GAP's timestamp and last_seen_us are
 * read before note_message overwrites them with this message's. */
gt_status gt_hub_accept(gt_hub *hub, uint32_t conn_id, const gt_msg *message,
                        const gt_hub_clock *now, gt_hub_result *result)
{
    if (hub == NULL || message == NULL || now == NULL || result == NULL ||
        !message_is_valid(message)) {
        return GT_ERR_INVAL;
    }
    sensor_track *track = &hub->sensors[message->sensor_id];
    gt_hub_result decided;
    memset(&decided, 0, sizeof decided);
    decided.events = counter_events(track, conn_id, message->counter);
    decided.expected_counter = track->expected_counter;
    decided.last_seen_us = track->seen ? track->last_seen_us : 0;
    if (track->stale) {
        decided.events |= GT_HUB_EV_RESUMED;
    }

    gt_status status = GT_OK;
    if (!value_in_range(message)) {
        decided.events |= GT_HUB_EV_READING_INVALID;
        status = GT_ERR_CORRUPT;
    } else if (!(decided.events & GT_HUB_EV_DUPLICATE)) {
        decided.has_record = 1;
        decided.record.type = message->type;   /* GT_MSG_* and gt_rec_type share values */
        decided.record.sensor_id = message->sensor_id;
        decided.record.value = message->value;
        decided.record.seq = 0;                /* stamped by the sender at send() */
        int gap_has_start = message->type == GT_MSG_GAP && track->has_data;
        decided.record.timestamp_us = gap_has_start ? track->last_data_us : now->wall_us;
    }
    note_message(track, conn_id, message, now, !(decided.events & GT_HUB_EV_DUPLICATE));
    *result = decided;
    return status;
}

/* Mark each sensor that has been quiet for its timeout as stale and
 * report it once (see hub.h). A stale sensor is skipped until its next
 * message clears `stale` (note_message), so one quiet spell gives one
 * report. The `now_ms >= last_seen_ms` test comes first because the
 * subtraction is unsigned: an earlier now_ms would wrap to a huge
 * number and look like a timeout. */
unsigned gt_hub_check_timeouts(gt_hub *hub, uint64_t now_ms)
{
    if (hub == NULL) {
        return 0;
    }
    unsigned timed_out = 0;
    /* Invariant: every sensor before sensor_id has been checked, and each
     * one that just went quiet is now stale with its bit in timed_out. */
    for (unsigned sensor_id = GT_SENSOR_LIGHT; sensor_id <= GT_SENSOR_SOIL; sensor_id++) {
        sensor_track *track = &hub->sensors[sensor_id];
        uint64_t timeout_ms = sensor_id == GT_SENSOR_LIGHT ? GT_HUB_LIGHT_TIMEOUT_MS
                                                           : GT_HUB_SOIL_TIMEOUT_MS;
        if (!track->stale && now_ms >= track->last_seen_ms &&
            now_ms - track->last_seen_ms >= timeout_ms) {
            track->stale = 1;
            timed_out |= 1u << sensor_id;
        }
    }
    return timed_out;
}

/* One row per GT_HUB_EV_* flag: the log name DESIGN.md's Hub rows use and
 * its level. Problems with the data (invalid, duplicate, lost, restarted,
 * quiet) are WARN; a sensor connecting or coming back is INFO. */
typedef struct {
    unsigned     flag;
    gt_hub_event event;
} hub_event_row;

static const hub_event_row HUB_EVENT_TABLE[] = {
    { GT_HUB_EV_READING_INVALID, { "reading_invalid",  GT_LOG_WARN } },
    { GT_HUB_EV_DUPLICATE,       { "ipc_duplicate",    GT_LOG_WARN } },
    { GT_HUB_EV_SEQ_GAP,         { "sensor_seq_gap",   GT_LOG_WARN } },
    { GT_HUB_EV_NEW_CLIENT,      { "sensor_connected", GT_LOG_INFO } },
    { GT_HUB_EV_SENSOR_RESTART,  { "sensor_restart",   GT_LOG_WARN } },
    { GT_HUB_EV_RESUMED,         { "sensor_resumed",   GT_LOG_INFO } },
    { GT_HUB_EV_TIMEOUT,         { "sensor_timeout",   GT_LOG_WARN } },
};

/* An exact match against one row's flag also rules out 0 and any word
 * with two bits set, since every row has exactly one bit (as in
 * gt_storage_event_describe). */
gt_status gt_hub_event_describe(unsigned flag, gt_hub_event *out)
{
    if (out == NULL) {
        return GT_ERR_INVAL;
    }
    size_t row_count = sizeof HUB_EVENT_TABLE / sizeof HUB_EVENT_TABLE[0];
    /* Invariant: no row before row_index has this flag. */
    for (size_t row_index = 0; row_index < row_count; row_index++) {
        if (HUB_EVENT_TABLE[row_index].flag == flag) {
            *out = HUB_EVENT_TABLE[row_index].event;
            return GT_OK;
        }
    }
    return GT_ERR_INVAL;
}

/* The hub owns no other memory, so this is just free(). NULL is a no-op
 * (free(NULL) is defined to do nothing). */
void gt_hub_destroy(gt_hub *hub)
{
    free(hub);
}
