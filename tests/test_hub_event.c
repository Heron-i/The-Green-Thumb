/* Unit tests for the hub's event names (src/hub.c): gt_hub_event_describe.
 *
 * Purpose: pin each GT_HUB_EV_* flag to the event name DESIGN.md's Hub
 * rows use and the level it is logged at, so the rx loop only loops over
 * the bits it got back. Mirrors storage's SEV tests. Pure lookup. Run
 * under make test / asan / memcheck.
 * Implements HEV-01 through HEV-03.
 *
 * What these tests cannot prove: that the rx loop logs every bit (HRX
 * does). */
#include <string.h>

#include "check.h"
#include "hub.h"

/* ---- HEV-01: every flag has its DESIGN.md name and level ---- */
static void test_hev01_names_and_levels(void)
{
    /* One expected row per flag, in the order hub.h defines them. */
    const struct {
        unsigned     flag;
        const char  *name;
        gt_log_level level;
    } expected[] = {
        { GT_HUB_EV_READING_INVALID, "reading_invalid",  GT_LOG_WARN },
        { GT_HUB_EV_DUPLICATE,       "ipc_duplicate",    GT_LOG_WARN },
        { GT_HUB_EV_SEQ_GAP,         "sensor_seq_gap",   GT_LOG_WARN },
        { GT_HUB_EV_NEW_CLIENT,      "sensor_connected", GT_LOG_INFO },
        { GT_HUB_EV_SENSOR_RESTART,  "sensor_restart",   GT_LOG_WARN },
        { GT_HUB_EV_RESUMED,         "sensor_resumed",   GT_LOG_INFO },
        { GT_HUB_EV_TIMEOUT,         "sensor_timeout",   GT_LOG_WARN },
    };

    /* Invariant: expected[0..row_index-1] each described as listed. */
    for (size_t row_index = 0; row_index < sizeof expected / sizeof expected[0]; row_index++) {
        gt_hub_event event = { NULL, GT_LOG_DEBUG };
        CHECK_EQ(gt_hub_event_describe(expected[row_index].flag, &event), GT_OK);
        CHECK(event.name != NULL && strcmp(event.name, expected[row_index].name) == 0);
        CHECK_EQ(event.level, expected[row_index].level);
    }
}

/* ---- HEV-02: 0 or two bits at once are rejected ----
 * The caller must describe one bit at a time; *out keeps its old value. */
static void test_hev02_zero_or_two_bits(void)
{
    gt_hub_event event = { "untouched", GT_LOG_ERROR };

    CHECK_EQ(gt_hub_event_describe(0, &event), GT_ERR_INVAL);
    CHECK_EQ(gt_hub_event_describe(GT_HUB_EV_NEW_CLIENT | GT_HUB_EV_SENSOR_RESTART, &event),
             GT_ERR_INVAL);
    CHECK(strcmp(event.name, "untouched") == 0);
    CHECK_EQ(event.level, GT_LOG_ERROR);
}

/* ---- HEV-03: an unknown bit or a NULL out is rejected ----
 * 0x80 is the first bit hub.h doesn't define. */
static void test_hev03_unknown_or_null(void)
{
    gt_hub_event event = { "untouched", GT_LOG_ERROR };

    CHECK_EQ(gt_hub_event_describe(0x80u, &event), GT_ERR_INVAL);
    CHECK(strcmp(event.name, "untouched") == 0);
    CHECK_EQ(gt_hub_event_describe(GT_HUB_EV_SEQ_GAP, NULL), GT_ERR_INVAL);
}

static const test_case_t tests[] = {
    {"HEV-01", "every flag has its DESIGN.md name and level", test_hev01_names_and_levels},
    {"HEV-02", "0 or two bits at once are rejected", test_hev02_zero_or_two_bits},
    {"HEV-03", "an unknown bit or a NULL out is rejected", test_hev03_unknown_or_null},
};

int main(void)
{
    return RUN_TESTS(tests);
}
