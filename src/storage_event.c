/* Log names for storage's events (see storage.h, gt_storage_event).
 *
 * Purpose: one table from each GT_STORAGE_EV_* flag to the event name and
 * level DESIGN.md's failure-mode table uses, so storage_main only loops
 * over the bits it got back and logs each one. Like sensor_health.c's
 * event names, it decides nothing and does no I/O.
 *
 * Pure lookup: no files. */
#include "storage.h"

#include <stddef.h>

/* One row per flag. */
typedef struct {
    unsigned         flag;
    gt_storage_event event;
} event_row;

static const event_row EVENT_TABLE[] = {
    { GT_STORAGE_EV_SEQ_REJECTED,         { "seq_rejected",         GT_LOG_WARN,  NULL } },
    { GT_STORAGE_EV_RECOVERY_GAP,         { "recovery_gap",         GT_LOG_WARN,  NULL } },
    { GT_STORAGE_EV_WRITE_ERROR,          { "storage_write_error",  GT_LOG_ERROR, NULL } },
    { GT_STORAGE_EV_FSYNC_ERROR,          { "fsync_error",          GT_LOG_ERROR, NULL } },
    { GT_STORAGE_EV_DEGRADED,             { "storage_degraded",     GT_LOG_ERROR, "write_failed" } },
    { GT_STORAGE_EV_RECOVERED,            { "storage_recovered",    GT_LOG_INFO,  NULL } },
    { GT_STORAGE_EV_BACKLOG,              { "storage_backlog",      GT_LOG_WARN,  NULL } },
    { GT_STORAGE_EV_QUEUE_FULL,           { "storage_degraded",     GT_LOG_ERROR, "queue_full" } },
    { GT_STORAGE_EV_ROLLOVER,             { "segment_rollover",     GT_LOG_INFO,  NULL } },
    { GT_STORAGE_EV_SEGMENT_CREATE_ERROR, { "segment_create_error", GT_LOG_ERROR, NULL } },
    { GT_STORAGE_EV_RETENTION_SKIPPED,    { "retention_skipped",    GT_LOG_INFO,  NULL } },
    { GT_STORAGE_EV_RETENTION_ERROR,      { "retention_error",      GT_LOG_WARN,  NULL } },
    { GT_STORAGE_EV_BAD_MESSAGE,          { "ipc_bad_message",      GT_LOG_WARN,  NULL } },
};

/* An exact match against one row's flag also rules out 0 and any word
 * with two bits set, since every row has exactly one bit. */
gt_status gt_storage_event_describe(unsigned flag, gt_storage_event *out)
{
    if (out == NULL) {
        return GT_ERR_INVAL;
    }
    size_t row_count = sizeof EVENT_TABLE / sizeof EVENT_TABLE[0];
    for (size_t row_index = 0; row_index < row_count; row_index++) {
        if (EVENT_TABLE[row_index].flag == flag) {
            *out = EVENT_TABLE[row_index].event;
            return GT_OK;
        }
    }
    return GT_ERR_INVAL;
}
