/* Unit tests for the shared log line formatter (src/gt_log.c):
 * gt_log_format.
 *
 * Purpose: prove every line matches DESIGN.md's one format,
 * "timestamp level component event key=value", that a line that does not
 * fit is reported instead of silently cut, and that input which would make
 * a line unparseable is rejected. Pure formatting: no clock, no file.
 * Run under make test / asan / memcheck. Implements LGF-01 through LGF-08.
 *
 * What these tests cannot prove: where lines end up (stderr, a file or the
 * monitor). That is decided with the light_sensor process. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "gt_log.h"

/* 2023-11-14 in µs since the epoch; any fixed value works. The ull suffix
 * makes the literal an unsigned long long, wide enough for 16 digits. */
#define SAMPLE_TIME_US 1700000000000000ull

/* ---- LGF-01: a full line has every field in order ---- */
static void test_lgf01_full_line(void)
{
    char line[GT_LOG_LINE_MAX];
    CHECK_EQ(gt_log_format(line, sizeof line, SAMPLE_TIME_US, GT_LOG_WARN,
                           "light_sensor", "sensor_offline", "fails=10"), GT_OK);
    CHECK(strcmp(line, "1700000000000000 WARN light_sensor sensor_offline fails=10") == 0);
}

/* ---- LGF-02: no fields means no trailing space ----
 * NULL and "" both mean "no key=value pairs". */
static void test_lgf02_no_fields_no_trailing_space(void)
{
    char line[GT_LOG_LINE_MAX];
    const char *expected_line = "1700000000000000 INFO light_sensor sensor_online";

    CHECK_EQ(gt_log_format(line, sizeof line, SAMPLE_TIME_US, GT_LOG_INFO,
                           "light_sensor", "sensor_online", NULL), GT_OK);
    CHECK(strcmp(line, expected_line) == 0);
    CHECK_EQ(gt_log_format(line, sizeof line, SAMPLE_TIME_US, GT_LOG_INFO,
                           "light_sensor", "sensor_online", ""), GT_OK);
    CHECK(strcmp(line, expected_line) == 0);
}

/* ---- LGF-03: each level prints its name ---- */
static void test_lgf03_level_names(void)
{
    const gt_log_level levels[] = { GT_LOG_DEBUG, GT_LOG_INFO, GT_LOG_WARN, GT_LOG_ERROR };
    const char *expected_lines[] = { "0 DEBUG c e", "0 INFO c e", "0 WARN c e", "0 ERROR c e" };
    size_t level_count = sizeof levels / sizeof levels[0];
    char line[GT_LOG_LINE_MAX];

    for (size_t level_index = 0; level_index < level_count; level_index++) {
        CHECK_EQ(gt_log_format(line, sizeof line, 0, levels[level_index], "c", "e", NULL),
                 GT_OK);
        CHECK(strcmp(line, expected_lines[level_index]) == 0);
    }
}

/* ---- LGF-04: timestamps print in full at both ends ----
 * 0 and the largest uint64_t (20 digits): nothing is cut or wrapped. */
static void test_lgf04_timestamp_range(void)
{
    char line[GT_LOG_LINE_MAX];

    CHECK_EQ(gt_log_format(line, sizeof line, 0, GT_LOG_INFO, "c", "e", NULL), GT_OK);
    CHECK(strcmp(line, "0 INFO c e") == 0);
    CHECK_EQ(gt_log_format(line, sizeof line, UINT64_MAX, GT_LOG_INFO, "c", "e", NULL), GT_OK);
    CHECK(strcmp(line, "18446744073709551615 INFO c e") == 0);
}

/* ---- LGF-05: a line that doesn't fit is reported ----
 * A cut-off line must not pass as a whole one, and the buffer must still
 * end in '\0' so a caller that prints it anyway can't read past the end.
 * memchr looks for a '\0' byte within the buffer's 8 bytes. */
static void test_lgf05_too_small_is_full(void)
{
    char small_line[8];
    CHECK_EQ(gt_log_format(small_line, sizeof small_line, SAMPLE_TIME_US, GT_LOG_WARN,
                           "light_sensor", "sensor_offline", NULL), GT_ERR_FULL);
    CHECK(memchr(small_line, '\0', sizeof small_line) != NULL);
}

/* ---- LGF-06: missing arguments are rejected ----
 * NULL buffer, zero-size buffer (no room even for '\0'), NULL component
 * or NULL event. */
static void test_lgf06_missing_args(void)
{
    char line[GT_LOG_LINE_MAX];

    CHECK_EQ(gt_log_format(NULL, sizeof line, 0, GT_LOG_INFO, "c", "e", NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_log_format(line, 0, 0, GT_LOG_INFO, "c", "e", NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_log_format(line, sizeof line, 0, GT_LOG_INFO, NULL, "e", NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_log_format(line, sizeof line, 0, GT_LOG_INFO, "c", NULL, NULL), GT_ERR_INVAL);
}

/* ---- LGF-07: a space in component or event is rejected ----
 * Lines are split on spaces, so "light sensor" would parse as two fields.
 * Spaces inside `fields` are fine: they separate the key=value pairs. */
static void test_lgf07_space_in_name_rejected(void)
{
    char line[GT_LOG_LINE_MAX];

    CHECK_EQ(gt_log_format(line, sizeof line, 0, GT_LOG_INFO, "light sensor", "e", NULL),
             GT_ERR_INVAL);
    CHECK_EQ(gt_log_format(line, sizeof line, 0, GT_LOG_INFO, "c", "sensor offline", NULL),
             GT_ERR_INVAL);
    CHECK_EQ(gt_log_format(line, sizeof line, 0, GT_LOG_INFO, "c", "e", "a=1 b=2"), GT_OK);
}

/* ---- LGF-08: an unknown level is rejected ----
 * Like SHE-04: a stray int cast to the enum must not index past the
 * level-name table. */
static void test_lgf08_unknown_level(void)
{
    char line[GT_LOG_LINE_MAX];
    CHECK_EQ(gt_log_format(line, sizeof line, 0, (gt_log_level)99, "c", "e", NULL),
             GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"LGF-01", "a full line has every field in order", test_lgf01_full_line},
    {"LGF-02", "no fields means no trailing space", test_lgf02_no_fields_no_trailing_space},
    {"LGF-03", "each level prints its name", test_lgf03_level_names},
    {"LGF-04", "timestamps print in full at both ends", test_lgf04_timestamp_range},
    {"LGF-05", "a line that doesn't fit is reported", test_lgf05_too_small_is_full},
    {"LGF-06", "missing arguments are rejected", test_lgf06_missing_args},
    {"LGF-07", "a space in component or event is rejected", test_lgf07_space_in_name_rejected},
    {"LGF-08", "an unknown level is rejected", test_lgf08_unknown_level},
};

int main(void)
{
    return RUN_TESTS(tests);
}
