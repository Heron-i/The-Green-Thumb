/* Segment naming and listing (see storage.h).
 *
 * Purpose: which file a record for a given UTC day goes in, which files
 * in the directory are segments at all, and the one oldest-first order
 * storage, retention and the verifier read them in (DESIGN.md "How
 * records are organized"). Nothing here touches the storage object, so
 * it works on any directory behind a gt_file_ops. */
#include "storage.h"

#include <string.h>

#define US_PER_S     1000000u
#define S_PER_DAY    86400u
/* 9999-12-31 23:59:59 UTC, the last second whose year fits 4 digits. */
#define MAX_NAMED_S  253402300799u

#define SEGMENT_PREFIX     "log-"
#define SEGMENT_SUFFIX     ".bin"
#define SEGMENT_PREFIX_LEN 4
#define SEGMENT_DIGITS     8
#define SEGMENT_NAME_LEN   (SEGMENT_PREFIX_LEN + SEGMENT_DIGITS + 4)   /* 16 */

/* Turn a count of days since 1970-01-01 into a year, month (1-12) and day
 * (1-31) in the Gregorian calendar. This is Howard Hinnant's public-domain
 * "civil_from_days" algorithm, restricted to dates from 1970 on, so every
 * value is unsigned.
 *
 * Why not gmtime_r: on 32-bit Raspberry Pi OS, time_t is 32 bits, so
 * gmtime_r fails for every date after January 2038. This needs only
 * integer arithmetic and works the same on every platform.
 *
 * How it works: it shifts the calendar so each year starts on 1 March
 * (so the leap day is the last day of the year), then splits the days
 * into 400-year "eras" of exactly 146097 days, since the leap-year
 * pattern repeats every 400 years. Within an era it finds the year, then
 * the day of that year, then the month from a formula that fits the
 * month lengths starting from March (31, 30, 31, 30, 31, 31, 30, ...). */
static void civil_from_days(uint64_t days_since_1970, uint64_t *year_out,
                            unsigned *month_out, unsigned *day_out)
{
    uint64_t days = days_since_1970 + 719468;   /* days since 0000-03-01 */
    uint64_t era = days / 146097;
    uint64_t day_of_era = days - era * 146097;                       /* 0 .. 146096 */
    uint64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524
                            - day_of_era / 146096) / 365;             /* 0 .. 399 */
    uint64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4
                                         - year_of_era / 100);       /* 0 .. 365 */
    uint64_t month_from_march = (5 * day_of_year + 2) / 153;         /* 0 .. 11 */

    unsigned month = (unsigned)(month_from_march < 10 ? month_from_march + 3
                                                      : month_from_march - 9);
    *day_out = (unsigned)(day_of_year - (153 * month_from_march + 2) / 5 + 1);
    *month_out = month;
    *year_out = year_of_era + era * 400 + (month <= 2);   /* Jan and Feb belong to the next year */
}

/* Write `value` as exactly `width` decimal digits, with leading zeros,
 * into out[0 .. width-1]. The caller makes sure value fits. */
static void put_digits(char *out, uint64_t value, unsigned width)
{
    /* invariant: out[position .. width - 1] already hold value's lowest
     * digits, and value has had those digits divided off */
    for (unsigned position = width; position > 0; position--) {
        out[position - 1] = (char)('0' + value % 10);
        value /= 10;
    }
}

gt_status gt_segment_name(uint64_t now_us, char name_out[GT_FILE_NAME_MAX])
{
    uint64_t seconds = now_us / US_PER_S;
    if (name_out == NULL || seconds > MAX_NAMED_S) {
        return GT_ERR_INVAL;
    }

    uint64_t year;
    unsigned month, day;
    civil_from_days(seconds / S_PER_DAY, &year, &month, &day);

    char name[SEGMENT_NAME_LEN + 1];
    memcpy(name, SEGMENT_PREFIX, SEGMENT_PREFIX_LEN);
    put_digits(&name[4], year, 4);
    put_digits(&name[8], month, 2);
    put_digits(&name[10], day, 2);
    memcpy(&name[12], SEGMENT_SUFFIX, sizeof SEGMENT_SUFFIX);   /* includes the NUL */

    memcpy(name_out, name, sizeof name);
    return GT_OK;
}

/* Digits are checked as '0'..'9' directly rather than with isdigit(),
 * which depends on the locale and is undefined for negative char values. */
int gt_is_segment_name(const char *name)
{
    if (name == NULL || strlen(name) != SEGMENT_NAME_LEN
        || memcmp(name, SEGMENT_PREFIX, SEGMENT_PREFIX_LEN) != 0
        || strcmp(&name[SEGMENT_PREFIX_LEN + SEGMENT_DIGITS], SEGMENT_SUFFIX) != 0) {
        return 0;
    }
    for (size_t digit_index = 0; digit_index < SEGMENT_DIGITS; digit_index++) {
        char digit = name[SEGMENT_PREFIX_LEN + digit_index];
        if (digit < '0' || digit > '9') {
            return 0;
        }
    }
    return 1;
}

/* Declared in storage.h: storage, retention and the verifier all read the
 * segments in this one order. */
gt_status gt_storage_list_segments(const gt_file_ops *ops, void *ctx,
                                   char segments[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX],
                                   size_t *count_out, int *marker_out)
{
    char names[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX];
    size_t name_count = 0;
    if (ops->list(ctx, names, &name_count) != GT_OK) {
        return GT_ERR_IO;
    }

    size_t count = 0;
    *marker_out = 0;
    for (size_t name_index = 0; name_index < name_count; name_index++) {
        if (strcmp(names[name_index], GT_STORAGE_MARKER_NAME) == 0) {
            *marker_out = 1;
        } else if (gt_is_segment_name(names[name_index])) {
            memcpy(segments[count], names[name_index], GT_FILE_NAME_MAX);
            count++;
        }
    }

    /* Insertion sort: there are at most about 31 segments.
     * invariant: segments[0 .. sorted - 1] are in order */
    for (size_t sorted = 1; sorted < count; sorted++) {
        char moving[GT_FILE_NAME_MAX];
        memcpy(moving, segments[sorted], GT_FILE_NAME_MAX);
        size_t slot = sorted;
        /* invariant: segments[slot + 1 .. sorted] are all greater than moving */
        while (slot > 0 && strcmp(segments[slot - 1], moving) > 0) {
            memcpy(segments[slot], segments[slot - 1], GT_FILE_NAME_MAX);
            slot--;
        }
        memcpy(segments[slot], moving, GT_FILE_NAME_MAX);
    }
    *count_out = count;
    return GT_OK;
}

/* Uses the same listing open uses, so "empty" means exactly "open would
 * name a new segment from the clock". */
gt_status gt_storage_log_empty_ops(const gt_file_ops *ops, void *ctx, int *empty_out)
{
    if (ops == NULL || empty_out == NULL) {
        return GT_ERR_INVAL;
    }
    char segments[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX];
    size_t segment_count = 0;
    int marker_present = 0;   /* not needed here */
    if (gt_storage_list_segments(ops, ctx, segments, &segment_count, &marker_present) != GT_OK) {
        return GT_ERR_IO;
    }
    *empty_out = segment_count == 0;
    return GT_OK;
}

/* The ctx is this function's own, so it is destroyed on every path. */
gt_status gt_storage_log_empty(const char *dir, int *empty_out)
{
    const gt_file_ops *ops = NULL;
    void *ctx = NULL;
    gt_status status = gt_file_ops_linux(dir, &ops, &ctx);
    if (status != GT_OK) {
        return status;
    }
    status = gt_storage_log_empty_ops(ops, ctx, empty_out);
    ops->destroy(ctx);
    return status;
}
