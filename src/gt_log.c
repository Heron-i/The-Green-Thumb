/* Shared log line formatter (see gt_log.h).
 *
 * Purpose: builds the one log line format DESIGN.md sets for every
 * component, "timestamp level component event key=value", so every process
 * logs lines that a single script can parse. It only fills a caller buffer:
 * no clock, no file, no system calls. Where lines go (stderr, a file or the
 * monitor) is decided by the process that calls it. */
#include "gt_log.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Indexed by gt_log_level; the order must match the enum. */
static const char *const level_names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
#define LEVEL_COUNT (sizeof level_names / sizeof level_names[0])

/* 1 if gt_log_format's arguments can make a valid line, 0 if not. */
static int log_args_valid(const char *line_out, size_t line_size, const gt_log_entry *entry)
{
    if (line_out == NULL || line_size == 0 || entry == NULL) {
        return 0;
    }
    if (entry->component == NULL || entry->event == NULL) {
        return 0;
    }
    /* Cast to unsigned so a negative stray value is also out of range. */
    if ((unsigned)entry->level >= LEVEL_COUNT) {
        return 0;
    }
    /* A space would split one field into two when the line is parsed. */
    return strchr(entry->component, ' ') == NULL && strchr(entry->event, ' ') == NULL;
}

/* Format *entry as one line into line_out. snprintf never writes more than line_size
 * bytes and always ends with '\0', and it returns the length the whole line
 * would have had, so a result >= line_size means the line was cut:
 * GT_ERR_FULL. PRIu64 (from <inttypes.h>) is the printf code for uint64_t. */
gt_status gt_log_format(char *line_out, size_t line_size, const gt_log_entry *entry)
{
    if (!log_args_valid(line_out, line_size, entry)) {
        return GT_ERR_INVAL;
    }

    int has_fields = entry->fields != NULL && entry->fields[0] != '\0';
    int line_length = snprintf(line_out, line_size, "%" PRIu64 " %s %s %s%s%s",
                               entry->timestamp_us, level_names[entry->level],
                               entry->component, entry->event,
                               has_fields ? " " : "", has_fields ? entry->fields : "");
    if (line_length < 0) {
        return GT_ERR_INVAL;   /* snprintf could not format at all */
    }
    return (size_t)line_length >= line_size ? GT_ERR_FULL : GT_OK;
}
