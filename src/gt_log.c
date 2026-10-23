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

/* Format one line into line_out. snprintf never writes more than line_size
 * bytes and always ends with '\0', and it returns the length the whole line
 * would have had, so a result >= line_size means the line was cut:
 * GT_ERR_FULL. PRIu64 (from <inttypes.h>) is the printf code for uint64_t. */
gt_status gt_log_format(char *line_out, size_t line_size, uint64_t timestamp_us,
                        gt_log_level level, const char *component,
                        const char *event, const char *fields)
{
    if (line_out == NULL || line_size == 0 || component == NULL || event == NULL) {
        return GT_ERR_INVAL;
    }
    /* Cast to unsigned so a negative stray value is also out of range. */
    if ((unsigned)level >= LEVEL_COUNT) {
        return GT_ERR_INVAL;
    }
    /* A space would split one field into two when the line is parsed. */
    if (strchr(component, ' ') != NULL || strchr(event, ' ') != NULL) {
        return GT_ERR_INVAL;
    }

    int has_fields = fields != NULL && fields[0] != '\0';
    int line_length = snprintf(line_out, line_size, "%" PRIu64 " %s %s %s%s%s",
                               timestamp_us, level_names[level], component, event,
                               has_fields ? " " : "", has_fields ? fields : "");
    if (line_length < 0) {
        return GT_ERR_INVAL;   /* snprintf could not format at all */
    }
    if ((size_t)line_length >= line_size) {
        return GT_ERR_FULL;
    }
    return GT_OK;
}
