/* Unit tests for opening and closing the hub's hop-2 sender
 * (src/hub_sender.c): gt_hub_sender_open_ops and gt_hub_sender_close.
 *
 * Purpose: prove the sender starts quietly and owns its socket ctx the
 * same way the sensor client does (Sensor-Pipeline.md Decisions,
 * "Ownership of the ctx"). Opening makes no socket call, since storage may
 * not be up yet (Mechanism E). Close calls destroy exactly once and sends
 * nothing more. Runs against tests/fake_sock.h. Run under make test /
 * asan / memcheck.
 * Implements HSO-01 through HSO-03.
 *
 * What these tests cannot prove: that close frees everything (memcheck
 * does), or anything about the reply and sending (HSR, HSS). */
#include <stdint.h>

#include "check.h"
#include "fake_sock.h"
#include "hub_sender.h"

/* ---- HSO-01: open makes no socket call ---- */
static void test_hso01_open_is_quiet(void)
{
    fake_sock fake;
    gt_hub_sender *sender = gt_hub_sender_open_ops(fake_sock_ops(&fake), &fake);

    CHECK(sender != NULL);
    CHECK_EQ(fake.connect_calls, 0);
    CHECK_EQ(fake.recv_calls, 0);
    CHECK_EQ(fake.send_calls, 0);
    CHECK_EQ(fake.destroy_calls, 0);
    gt_hub_send_stats stats;
    CHECK_EQ(gt_hub_sender_stats(sender, &stats), GT_OK);
    CHECK_EQ(stats.queued, 0);
    CHECK_EQ(stats.resumed, 0);
    CHECK_EQ(stats.has_own, 0);
    gt_hub_sender_close(sender);
}

/* ---- HSO-02: NULL ops returns NULL, ctx untouched ----
 * With no ops there is no destroy to call, so the caller still owns the
 * ctx: the fake must see no call at all. */
static void test_hso02_null_ops(void)
{
    fake_sock fake;
    fake_sock_ops(&fake);

    CHECK(gt_hub_sender_open_ops(NULL, &fake) == NULL);
    CHECK_EQ(fake.destroy_calls, 0);
    CHECK_EQ(fake.connect_calls, 0);
}

/* ---- HSO-03: close destroys once and sends nothing ----
 * A record is queued but never pumped; close drops it without a send, and
 * calls destroy exactly once (memcheck shows nothing leaks). */
static void test_hso03_close_destroys_once(void)
{
    fake_sock fake;
    gt_hub_sender *sender = gt_hub_sender_open_ops(fake_sock_ops(&fake), &fake);
    const gt_record unstamped = { .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
                                  .seq = 0, .timestamp_us = 1700000000000000ull, .value = 7 };

    CHECK_EQ(gt_hub_sender_push(sender, &unstamped, NULL), GT_OK);
    gt_hub_sender_close(sender);
    CHECK_EQ(fake.destroy_calls, 1);
    CHECK_EQ(fake.send_calls, 0);
    gt_hub_sender_close(NULL);
}

static const test_case_t tests[] = {
    {"HSO-01", "open makes no socket call", test_hso01_open_is_quiet},
    {"HSO-02", "NULL ops returns NULL, ctx untouched", test_hso02_null_ops},
    {"HSO-03", "close destroys once and sends nothing", test_hso03_close_destroys_once},
};

int main(void)
{
    return RUN_TESTS(tests);
}
