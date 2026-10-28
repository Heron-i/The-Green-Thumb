#ifndef HUB_H
#define HUB_H

#include <stdint.h>
#include "ipc.h"
#include "log_record.h"

/* The hub's per-message logic (DESIGN.md failure-mode table, Hub rows;
 * Sensor-Pipeline.md, Hub contract (hop 2) and Hub steps).
 *
 * gt_hub_accept checks one decoded sensor message and turns it into the
 * record the hub queues for storage: type, sensor, value and the receive
 * time are filled in, but seq stays 0 and there is no CRC yet. The hop-2
 * sender stamps seq + CRC just before send(), because the next seq isn't
 * known until storage's seq_resume reply arrives (PF-01).
 *
 * Pure logic, like the health tracker: the time is passed in, nothing is
 * logged and no system call is made. Events come back as bit flags and
 * the caller (the hub's rx loop) logs them. */

/* Range check (DESIGN.md Hub row "Out-of-range reading"): raw counts above
 * these are reading_invalid and are not stored. Soil rejects only the
 * seesaw's 0xFFFF error code (Sensor-Pipeline.md Decisions); Lucas can
 * tighten it once the soil driver exists. The `u` suffix makes the
 * constant unsigned, matching the uint32_t value it is compared with. */
#define GT_HUB_LIGHT_RAW_MAX 0xFFFFu
#define GT_HUB_SOIL_RAW_MAX  0xFFFEu

/* Last-seen timeouts (DESIGN.md Hub row "Sensor goes quiet"), used by
 * gt_hub_check_timeouts. */
#define GT_HUB_LIGHT_TIMEOUT_MS 3000u
#define GT_HUB_SOIL_TIMEOUT_MS  180000u

/* Event flags, one bit each, ORed together in gt_hub_result.events (the
 * same pattern as GT_IPC_EV_* in ipc.h). The caller logs the name in
 * brackets. */
#define GT_HUB_EV_READING_INVALID 0x01u  /* value out of range: reading_invalid */
#define GT_HUB_EV_DUPLICATE       0x02u  /* counter already seen: ipc_duplicate */
#define GT_HUB_EV_SEQ_GAP         0x04u  /* counter jumped ahead: sensor_seq_gap */
#define GT_HUB_EV_NEW_CLIENT      0x08u  /* first message on this connection for
                                          * this sensor; the rx loop closes any
                                          * older connection for it
                                          * (sensor_duplicate_client) */
#define GT_HUB_EV_SENSOR_RESTART  0x10u  /* new connection whose counter went back:
                                          * sensor_restart, log only, no GAP */
#define GT_HUB_EV_RESUMED         0x20u  /* first message after a timeout:
                                          * sensor_resumed */

/* The two clocks the hub reads, passed together. wall_us (CLOCK_REALTIME,
 * us since the epoch) is stamped into records; mono_ms (CLOCK_MONOTONIC)
 * drives the last-seen timeouts and never jumps when NTP sets the clock. */
typedef struct {
    uint64_t wall_us;
    uint64_t mono_ms;
} gt_hub_clock;

/* What gt_hub_accept decided about one message. */
typedef struct {
    gt_record record;            /* valid only when has_record is 1; seq is 0 */
    int       has_record;        /* 1: queue `record` for storage; 0: nothing to queue */
    unsigned  events;            /* GT_HUB_EV_* flags */
    uint32_t  expected_counter;  /* the counter the hub expected from this sensor
                                  * (for the sensor_seq_gap / ipc_duplicate lines) */
    uint64_t  last_seen_us;      /* receive time of this sensor's previous message,
                                  * 0 if none (for the sensor_restart line) */
} gt_hub_result;

typedef struct gt_hub gt_hub;

/* A hub with no sensor seen yet. now_ms (monotonic) starts every sensor's
 * last-seen timer, so a sensor that never connects is still reported once
 * by gt_hub_check_timeouts. NULL if out of memory. */
gt_hub   *gt_hub_create(uint64_t now_ms);

/* Check one decoded message that arrived on connection `conn_id` and fill
 * *result. conn_id is a number the rx loop gives each accepted connection,
 * counting up (not the fd, which the kernel reuses after close).
 *   - DATA: record = {DATA, sensor, value, timestamp_us = now->wall_us,
 *     seq 0}.
 *   - GAP (counters counter..counter+value-1 were refused by the sensor):
 *     record = {GAP, sensor, value, timestamp_us = this sensor's last DATA
 *     receive time, the earliest the gap could have started; now->wall_us
 *     if there was none}.
 *   - Same connection, counter below the expected one: dropped, has_record
 *     0, GT_HUB_EV_DUPLICATE. Above it: queued, GT_HUB_EV_SEQ_GAP.
 *   - A new conn_id for the sensor: GT_HUB_EV_NEW_CLIENT, plus
 *     GT_HUB_EV_SENSOR_RESTART if the counter went back (not a duplicate:
 *     the sensor process restarted) or GT_HUB_EV_SEQ_GAP if it jumped.
 * Every message from the sensor, even a rejected one, updates its
 * last-seen time and moves the expected counter past itself.
 *   GT_OK          *result filled (has_record may be 0 for a duplicate)
 *   GT_ERR_CORRUPT value out of range: *result filled with has_record 0
 *                  and GT_HUB_EV_READING_INVALID
 *   GT_ERR_INVAL   NULL args, or a type / sensor_id gt_msg_decode would
 *                  reject; *result untouched */
gt_status gt_hub_accept(gt_hub *hub, uint32_t conn_id, const gt_msg *message,
                        const gt_hub_clock *now, gt_hub_result *result);

/* Last-seen check (built in H3). Returns a bit mask with bit (1u <<
 * sensor_id) set for each sensor that has just gone quiet: nothing for
 * GT_HUB_LIGHT_TIMEOUT_MS / GT_HUB_SOIL_TIMEOUT_MS. A sensor is reported
 * once per quiet spell; its next message brings GT_HUB_EV_RESUMED. */
unsigned  gt_hub_check_timeouts(gt_hub *hub, uint64_t now_ms);

/* Free the hub. NULL is a no-op. */
void      gt_hub_destroy(gt_hub *hub);

#endif
