#ifndef GT_LOG_H
#define GT_LOG_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"

/* One log line format for every component (DESIGN.md failure-mode table):
 *   <timestamp_us> <LEVEL> <component> <event>[ key=value ...]
 * timestamp_us is µs since the Unix epoch, the same unit as the on-disk
 * record, so log lines and records can be joined. This only formats into a
 * caller buffer; where lines are written is up to the caller. */
typedef enum { GT_LOG_DEBUG, GT_LOG_INFO, GT_LOG_WARN, GT_LOG_ERROR } gt_log_level;

#define GT_LOG_LINE_MAX 256   /* caller buffer size that fits any normal line */

/* What one log line says, before it is formatted. A struct groups related
 * values under one name so they travel together as one argument. */
typedef struct {
    uint64_t timestamp_us;    /* µs since the Unix epoch */
    gt_log_level level;
    const char *component;    /* e.g. "storage"; no spaces */
    const char *event;        /* e.g. "sensor_offline"; no spaces */
    const char *fields;       /* caller-built "key=value key=value", or NULL/"" for none */
} gt_log_entry;

/* Write one line for *entry into line_out (always NUL-terminated when
 * line_size > 0).
 * GT_ERR_INVAL: NULL line_out/entry/component/event, line_size 0, unknown
 * level, or a space in component/event (it would break parsing).
 * GT_ERR_FULL: the line did not fit. */
gt_status gt_log_format(char *line_out, size_t line_size, const gt_log_entry *entry);

#endif
