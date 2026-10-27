/* Unit tests for naming storage's events (src/storage_event.c):
 * gt_storage_event_describe.
 *
 * Purpose: prove every GT_STORAGE_EV_* flag maps to the log event name and
 * level DESIGN.md's failure-mode table uses (Storage rows), that every
 * name is accepted by gt_log_format (no spaces), that the two
 * storage_degraded causes are told apart by reason, and that 0, unknown
 * and combined flags are refused so storage_main logs each bit on its
 * own. Pure lookup: no files. Run under make test / asan / memcheck.
 * Implements SEV-01 through SEV-05.
 *
 * What these tests cannot prove: that storage_main logs every event it
 * receives (Mechanism D Step 12d-3, checked in the WSL smoke run). */
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "gt_log.h"
#include "storage.h"

/* One expected mapping. */
typedef struct {
    unsigned     flag;
    const char  *name;
    gt_log_level level;
} expected_event;

static const expected_event EXPECTED[] = {
    { GT_STORAGE_EV_SEQ_REJECTED,         "seq_rejected",         GT_LOG_WARN },
    { GT_STORAGE_EV_RECOVERY_GAP,         "recovery_gap",         GT_LOG_WARN },
    { GT_STORAGE_EV_WRITE_ERROR,          "storage_write_error",  GT_LOG_ERROR },
    { GT_STORAGE_EV_FSYNC_ERROR,          "fsync_error",          GT_LOG_ERROR },
    { GT_STORAGE_EV_DEGRADED,             "storage_degraded",     GT_LOG_ERROR },
    { GT_STORAGE_EV_RECOVERED,            "storage_recovered",    GT_LOG_INFO },
    { GT_STORAGE_EV_BACKLOG,              "storage_backlog",      GT_LOG_WARN },
    { GT_STORAGE_EV_QUEUE_FULL,           "storage_degraded",     GT_LOG_ERROR },
    { GT_STORAGE_EV_ROLLOVER,             "segment_rollover",     GT_LOG_INFO },
    { GT_STORAGE_EV_SEGMENT_CREATE_ERROR, "segment_create_error", GT_LOG_ERROR },
    { GT_STORAGE_EV_RETENTION_SKIPPED,    "retention_skipped",    GT_LOG_INFO },
    { GT_STORAGE_EV_RETENTION_ERROR,      "retention_error",      GT_LOG_WARN },
    { GT_STORAGE_EV_BAD_MESSAGE,          "ipc_bad_message",      GT_LOG_WARN },
};
#define EXPECTED_COUNT (sizeof EXPECTED / sizeof EXPECTED[0])

/* ---- SEV-01: every flag has its name and level ---- */
static void test_sev01_names_and_levels(void)
{
    for (size_t event_index = 0; event_index < EXPECTED_COUNT; event_index++) {
        gt_storage_event event;
        CHECK_EQ(gt_storage_event_describe(EXPECTED[event_index].flag, &event), GT_OK);
        CHECK(event.name != NULL && strcmp(event.name, EXPECTED[event_index].name) == 0);
        CHECK_EQ(event.level, EXPECTED[event_index].level);
    }
}

/* ---- SEV-02: every name is a valid log event ----
 * gt_log_format refuses a space in an event name, so each name must go
 * through it. */
static void test_sev02_names_loggable(void)
{
    for (size_t event_index = 0; event_index < EXPECTED_COUNT; event_index++) {
        gt_storage_event event;
        char line[GT_LOG_LINE_MAX];
        CHECK_EQ(gt_storage_event_describe(EXPECTED[event_index].flag, &event), GT_OK);
        CHECK_EQ(gt_log_format(line, sizeof line, 1, event.level, "storage", event.name, NULL),
                 GT_OK);
    }
}

/* ---- SEV-03: the degraded causes differ by reason ----
 * A failed write and a full buffer are both storage_degraded; reason
 * tells them apart. Every other event has no reason. */
static void test_sev03_degraded_reasons(void)
{
    gt_storage_event event;

    CHECK_EQ(gt_storage_event_describe(GT_STORAGE_EV_DEGRADED, &event), GT_OK);
    CHECK(event.reason != NULL && strcmp(event.reason, "write_failed") == 0);
    CHECK_EQ(gt_storage_event_describe(GT_STORAGE_EV_QUEUE_FULL, &event), GT_OK);
    CHECK(event.reason != NULL && strcmp(event.reason, "queue_full") == 0);

    for (size_t event_index = 0; event_index < EXPECTED_COUNT; event_index++) {
        unsigned flag = EXPECTED[event_index].flag;
        if (flag == GT_STORAGE_EV_DEGRADED || flag == GT_STORAGE_EV_QUEUE_FULL) {
            continue;
        }
        CHECK_EQ(gt_storage_event_describe(flag, &event), GT_OK);
        CHECK(event.reason == NULL);
    }
}

/* ---- SEV-04: 0, unknown and combined are refused ----
 * GT_ERR_INVAL and the output untouched: an events word is logged one
 * bit at a time. */
static void test_sev04_bad_flags_refused(void)
{
    const unsigned bad_flags[] = {
        0,
        0x2000u,                                                    /* no such event */
        GT_STORAGE_EV_ROLLOVER | GT_STORAGE_EV_RETENTION_ERROR,     /* two at once */
    };
    for (size_t flag_index = 0; flag_index < 3; flag_index++) {
        gt_storage_event event, untouched;
        memset(&event, 0x5A, sizeof event);
        memset(&untouched, 0x5A, sizeof untouched);

        CHECK_EQ(gt_storage_event_describe(bad_flags[flag_index], &event), GT_ERR_INVAL);
        CHECK(memcmp(&event, &untouched, sizeof event) == 0);
    }
}

/* ---- SEV-05: a NULL output is rejected ---- */
static void test_sev05_null_out(void)
{
    CHECK_EQ(gt_storage_event_describe(GT_STORAGE_EV_ROLLOVER, NULL), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"SEV-01", "every flag has its name and level", test_sev01_names_and_levels},
    {"SEV-02", "every name is a valid log event", test_sev02_names_loggable},
    {"SEV-03", "the degraded causes differ by reason", test_sev03_degraded_reasons},
    {"SEV-04", "0, unknown and combined are refused", test_sev04_bad_flags_refused},
    {"SEV-05", "a NULL output is rejected", test_sev05_null_out},
};

int main(void)
{
    return RUN_TESTS(tests);
}
