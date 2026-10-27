/* Unit tests for segment file names (src/storage_segment.c): gt_segment_name and
 * gt_is_segment_name.
 *
 * Purpose: prove each UTC day maps to exactly one "log-YYYYMMDD.bin"
 * (DESIGN.md "one segment file per UTC day"), with the day changing at
 * midnight UTC and not a microsecond early, and that only names of exactly
 * that shape count as segments, so storage never recovers, appends to or
 * deletes a .corrupt file or the marker. The expected seconds were
 * computed outside this codebase, with Python's datetime in UTC. Pure
 * logic: no files. Run under make test / asan / memcheck. Implements
 * SNM-01 through SNM-08.
 *
 * What these tests cannot prove: that storage picks the right segment from
 * a directory listing. That is gt_storage_open_ops (Mechanism D Step 7b). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "storage.h"

#define US_PER_S 1000000u

/* Check gt_segment_name(now_us) gives `expected`. */
static void check_name(uint64_t now_us, const char *expected)
{
    char name[GT_FILE_NAME_MAX];
    memset(name, 0, sizeof name);

    CHECK_EQ(gt_segment_name(now_us, name), GT_OK);
    CHECK(strcmp(name, expected) == 0);
}

/* ---- SNM-01: the Unix epoch is 1970-01-01 ----
 * What a Pi with no network time and no RTC thinks the date is. */
static void test_snm01_epoch(void)
{
    check_name(0, "log-19700101.bin");
}

/* ---- SNM-02: a known day gets its name ----
 * 2026-10-26 00:00:00 UTC is 1792972800 s. */
static void test_snm02_known_day(void)
{
    check_name((uint64_t)1792972800u * US_PER_S, "log-20261026.bin");
}

/* ---- SNM-03: the day changes exactly at midnight ----
 * The last microsecond of 2026-10-26 is still the 26th; the next one is
 * the 27th. */
static void test_snm03_midnight_boundary(void)
{
    uint64_t midnight_us = (uint64_t)1793059200u * US_PER_S;

    check_name(midnight_us - 1, "log-20261026.bin");
    check_name(midnight_us, "log-20261027.bin");
}

/* ---- SNM-04: leap day and year 2000 ----
 * 2024-02-29 12:00 UTC (1709208000 s) and 2000-01-01 (946684800 s). */
static void test_snm04_leap_day_and_2000(void)
{
    check_name((uint64_t)1709208000u * US_PER_S, "log-20240229.bin");
    check_name((uint64_t)946684800u * US_PER_S, "log-20000101.bin");
}

/* ---- SNM-05: year 9999 fits, later is rejected ----
 * 9999-12-31 23:59:59 UTC is 253402300799 s, the last second that fits 4
 * digits. UINT64_MAX us is about the year 586,000: GT_ERR_INVAL, and the
 * name is untouched. */
static void test_snm05_year_limit(void)
{
    char name[GT_FILE_NAME_MAX], untouched[GT_FILE_NAME_MAX];
    memset(name, 'Z', sizeof name);
    memset(untouched, 'Z', sizeof untouched);

    check_name((uint64_t)253402300799u * US_PER_S, "log-99991231.bin");
    CHECK_EQ(gt_segment_name(UINT64_MAX, name), GT_ERR_INVAL);
    CHECK(memcmp(name, untouched, sizeof name) == 0);
}

/* ---- SNM-06: a NULL name_out is rejected ---- */
static void test_snm06_null_name_out(void)
{
    CHECK_EQ(gt_segment_name(0, NULL), GT_ERR_INVAL);
}

/* ---- SNM-07: segment names are recognized ----
 * Including one made by gt_segment_name itself, so the two can't drift
 * apart. */
static void test_snm07_segment_names_recognized(void)
{
    char made[GT_FILE_NAME_MAX];

    CHECK_EQ(gt_is_segment_name("log-20261026.bin"), 1);
    CHECK_EQ(gt_is_segment_name("log-19700101.bin"), 1);
    CHECK_EQ(gt_segment_name((uint64_t)1709208000u * US_PER_S, made), GT_OK);
    CHECK_EQ(gt_is_segment_name(made), 1);
}

/* ---- SNM-08: other names are not segments ----
 * Everything else storage may find in its directory, and near misses of
 * every part of the pattern. */
static void test_snm08_other_names_rejected(void)
{
    const char *not_segments[] = {
        "log-20261026.bin.corrupt",   /* renamed by recovery */
        "clean_shutdown",             /* the marker (Step 9) */
        "storage.sock.lock",
        "log-2026102.bin",            /* 7 digits */
        "log-202610261.bin",          /* 9 digits */
        "log-2026102a.bin",           /* a letter among the digits */
        "Log-20261026.bin",           /* wrong case */
        "log_20261026.bin",           /* wrong separator */
        "log-20261026.bi",            /* short suffix */
        "log-20261026.binx",          /* long suffix */
        "",
    };
    size_t name_count = sizeof not_segments / sizeof not_segments[0];

    for (size_t name_index = 0; name_index < name_count; name_index++) {
        CHECK_EQ(gt_is_segment_name(not_segments[name_index]), 0);
    }
    CHECK_EQ(gt_is_segment_name(NULL), 0);
}

static const test_case_t tests[] = {
    {"SNM-01", "the Unix epoch is 1970-01-01", test_snm01_epoch},
    {"SNM-02", "a known day gets its name", test_snm02_known_day},
    {"SNM-03", "the day changes exactly at midnight", test_snm03_midnight_boundary},
    {"SNM-04", "leap day and year 2000", test_snm04_leap_day_and_2000},
    {"SNM-05", "year 9999 fits, later is rejected", test_snm05_year_limit},
    {"SNM-06", "a NULL name_out is rejected", test_snm06_null_name_out},
    {"SNM-07", "segment names are recognized", test_snm07_segment_names_recognized},
    {"SNM-08", "other names are not segments", test_snm08_other_names_rejected},
};

int main(void)
{
    return RUN_TESTS(tests);
}
