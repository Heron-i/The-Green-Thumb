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

/* Write one line into line_out (always NUL-terminated when line_size > 0).
 * fields: caller-built "key=value key=value", or NULL/"" for none.
 * GT_ERR_INVAL: NULL line_out/component/event, line_size 0, unknown level,
 * or a space in component/event (it would break parsing).
 * GT_ERR_FULL: the line did not fit. */
gt_status gt_log_format(char *line_out, size_t line_size, uint64_t timestamp_us,
                        gt_log_level level, const char *component,
                        const char *event, const char *fields);

#endif
