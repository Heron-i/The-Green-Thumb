/* Unit tests for the light process's loop (src/light_loop.c) against a
 * fake bus and a fake hub socket: gt_light_loop_init and gt_light_loop_tick.
 *
 * Purpose: prove one tick does what DESIGN.md's Sensor rows ask, in order:
 * read when due, track health (i2c_read_error, sensor_offline,
 * sensor_online, sensor_reinit), send each good reading as one DATA
 * message with a counter the hub can check, flag light_saturated, pump the
 * client and log its events (ipc_disconnect, ipc_reconnect, ipc_overflow),
 * and keep the 1000 ms schedule anchored (sample_overrun). Time is passed
 * in, so nothing waits. Run under make test / asan / memcheck.
 * Implements LLP-01 through LLP-12.
 *
 * What these tests cannot prove: 1 Hz on a real clock and real jitter
 * (the main sleeps; that is checked on the Pi), or a real BH1750 and hub
 * (light_probe and the IPC ladder rungs 3-4). */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "fake_i2c.h"
#include "fake_sock.h"
#include "ipc_sock.h"
#include "light_loop.h"

#define BOOK_MAX 64

/* One log line the loop asked for. */
typedef struct {
    gt_log_level level;
    char         event[32];
    char         fields[64];
} logged_line;

/* Every line the loop logged, in order. */
typedef struct {
    size_t      count;
    logged_line lines[BOOK_MAX];
} log_book;

/* The log callback: copy the line into the log book passed as log_ctx. */
static void record_log(void *log_ctx, gt_log_level level, const char *event, const char *fields)
{
    log_book *book = log_ctx;
    if (book->count >= BOOK_MAX) {
        return;
    }
    logged_line *line = &book->lines[book->count++];
    line->level = level;
    snprintf(line->event, sizeof line->event, "%s", event);
    snprintf(line->fields, sizeof line->fields, "%s", fields != NULL ? fields : "");
}

/* How many logged lines were `event` at `level`. */
static size_t logged(const log_book *book, gt_log_level level, const char *event)
{
    size_t matches = 0;
    /* Invariant: matches counts book->lines[0..line_index-1] that are event at level. */
    for (size_t line_index = 0; line_index < book->count; line_index++) {
        const logged_line *line = &book->lines[line_index];
        if (line->level == level && strcmp(line->event, event) == 0) {
            matches++;
        }
    }
    return matches;
}

/* The fields of the first logged `event`, or NULL if it wasn't logged. */
static const char *logged_fields(const log_book *book, const char *event)
{
    /* Invariant: no line in book->lines[0..line_index-1] is `event`. */
    for (size_t line_index = 0; line_index < book->count; line_index++) {
        if (strcmp(book->lines[line_index].event, event) == 0) {
            return book->lines[line_index].fields;
        }
    }
    return NULL;
}

/* One test's light process: the fake bus and hub, the sensor and client
 * opened on them, the loop, and its log book. */
typedef struct {
    fake_i2c       bus;
    fake_sock      hub;
    log_book       book;
    gt_light_loop  loop;
} light_rig;

/* static: fake_sock keeps 1100 sent frames, about 35 KB. */
static light_rig rig;

/* Open the sensor and client on fresh fakes and start the loop at now_ms
 * with the first reading due then. The hub is up unless a test scripts
 * otherwise. */
static void start_rig(uint64_t now_ms)
{
    memset(&rig, 0, sizeof rig);
    gt_light_loop_ctx ctx = {
        .sensor  = gt_light_open_bus(fake_i2c_bus(&rig.bus)),
        .client  = gt_ipc_client_open_ops(fake_sock_ops(&rig.hub), &rig.hub),
        .log     = record_log,
        .log_ctx = &rig.book,
    };
    CHECK(ctx.sensor != NULL);
    CHECK(ctx.client != NULL);
    CHECK_EQ(gt_light_loop_init(&rig.loop, &ctx, now_ms), GT_OK);
}

/* Close what start_rig opened. */
static void stop_rig(void)
{
    gt_ipc_client_close(rig.loop.ctx.client);
    gt_light_close(rig.loop.ctx.sensor);
}

/* Script one good reading of `raw` (the BH1750 sends it high byte first). */
static void push_reading(uint32_t raw)
{
    const uint8_t data[2] = { (uint8_t)(raw >> 8), (uint8_t)(raw & 0xFFu) };
    fake_i2c_push_read(&rig.bus, data, sizeof data);
}

/* Decode the sent_index-th message the hub got. */
static gt_msg sent_message(size_t sent_index)
{
    gt_msg message;
    memset(&message, 0, sizeof message);
    CHECK_EQ(gt_msg_decode(rig.hub.sent[sent_index], &message), GT_OK);
    return message;
}

/* ---- LLP-01: a due tick reads and sends DATA counter 0 ----
 * The first tick is due at start: it reads 300 and the hub gets one DATA
 * message for the light sensor with counter 0, value 300 and seq 0 (the
 * hub stamps seq). The next reading is due one period later. A clean first
 * connect and a good read log nothing. */
static void test_llp01_due_tick_sends_data(void)
{
    start_rig(0);
    push_reading(300);

    uint64_t next_due = 0;
    CHECK_EQ(gt_light_loop_tick(&rig.loop, 0, &next_due), GT_OK);
    CHECK_EQ(next_due, GT_LIGHT_PERIOD_MS);
    CHECK_EQ(rig.hub.n_sent, 1);
    gt_msg message = sent_message(0);
    CHECK_EQ(message.type, GT_MSG_DATA);
    CHECK_EQ(message.sensor_id, GT_SENSOR_LIGHT);
    CHECK_EQ(message.counter, 0);
    CHECK_EQ(message.value, 300);
    CHECK_EQ(message.seq, 0);
    CHECK_EQ(rig.book.count, 0);

    stop_rig();
}

/* ---- LLP-02: the counter goes up one per reading ----
 * Three due ticks one period apart read 10, 20 and 30. The hub gets three
 * messages in order with counters 0, 1, 2 and those values, so it sees no
 * sensor_seq_gap; the loop's counter is then 3. */
static void test_llp02_counter_counts_up(void)
{
    start_rig(0);
    const uint32_t values[3] = { 10, 20, 30 };
    /* Invariant: ticks 0..reading_index-1 each read and sent one message. */
    for (size_t reading_index = 0; reading_index < 3; reading_index++) {
        push_reading(values[reading_index]);
        CHECK_EQ(gt_light_loop_tick(&rig.loop, reading_index * GT_LIGHT_PERIOD_MS, NULL), GT_OK);
    }

    CHECK_EQ(rig.hub.n_sent, 3);
    /* Invariant: messages 0..sent_index-1 matched their reading's counter and value. */
    for (size_t sent_index = 0; sent_index < 3; sent_index++) {
        gt_msg message = sent_message(sent_index);
        CHECK_EQ(message.counter, sent_index);
        CHECK_EQ(message.value, values[sent_index]);
    }
    CHECK_EQ(rig.loop.counter, 3);

    stop_rig();
}

/* ---- LLP-03: a tick before it's due doesn't read but pumps ----
 * The hub isn't up for the first tick, so its reading waits in the
 * client's ring. A tick at 500 ms (the next reading is due at 1000) reads
 * nothing, but pumps the client: the retry is due (100 ms backoff), so it
 * connects and the waiting message reaches the hub. The due time stays
 * anchored at 1000. */
static void test_llp03_not_due_still_pumps(void)
{
    start_rig(0);
    rig.hub.fail_connects = 1;
    push_reading(300);
    CHECK_EQ(gt_light_loop_tick(&rig.loop, 0, NULL), GT_OK);
    CHECK_EQ(rig.hub.n_sent, 0);
    int reads_before = rig.bus.read_calls;

    uint64_t next_due = 0;
    CHECK_EQ(gt_light_loop_tick(&rig.loop, 500, &next_due), GT_OK);
    CHECK_EQ(rig.bus.read_calls, reads_before);
    CHECK_EQ(rig.hub.n_sent, 1);
    CHECK_EQ(sent_message(0).value, 300);
    CHECK_EQ(next_due, GT_LIGHT_PERIOD_MS);

    stop_rig();
}

/* ---- LLP-04: a failed read logs i2c_read_error, sends nothing ----
 * Nothing is scripted, so the read fails after the driver's retries. The
 * tick logs one i2c_read_error at WARN (one line per failed read, not per
 * attempt) and nothing else, sends no message and leaves the counter at
 * 0. The next good reading is then counter 0, so the hub sees no gap. */
static void test_llp04_failed_read_logs_error(void)
{
    start_rig(0);
    CHECK_EQ(gt_light_loop_tick(&rig.loop, 0, NULL), GT_OK);
    CHECK_EQ(rig.bus.read_calls, 1 + GT_LIGHT_RETRIES);
    CHECK_EQ(rig.book.count, 1);
    CHECK_EQ(logged(&rig.book, GT_LOG_WARN, GT_HEALTH_I2C_READ_ERROR), 1);
    CHECK_EQ(rig.hub.n_sent, 0);
    CHECK_EQ(rig.loop.counter, 0);

    push_reading(300);
    CHECK_EQ(gt_light_loop_tick(&rig.loop, GT_LIGHT_PERIOD_MS, NULL), GT_OK);
    CHECK_EQ(rig.hub.n_sent, 1);
    CHECK_EQ(sent_message(0).counter, 0);

    stop_rig();
}

/* Fail GT_HEALTH_OFFLINE_AFTER due ticks in a row, one period apart from
 * 0 ms, so the sensor is offline. Returns when the first probe is due:
 * GT_HEALTH_RETRY_MS after the last failure. */
static uint64_t go_offline(void)
{
    uint64_t now_ms = 0;
    /* Invariant: ticks 0..fail_index-1 each failed one read and logged it. */
    for (int fail_index = 0; fail_index < GT_HEALTH_OFFLINE_AFTER; fail_index++) {
        now_ms = (uint64_t)fail_index * GT_LIGHT_PERIOD_MS;
        CHECK_EQ(gt_light_loop_tick(&rig.loop, now_ms, NULL), GT_OK);
    }
    return now_ms + GT_HEALTH_RETRY_MS;
}

/* Tick once a period from just after the last failure up to (not including)
 * probe_at_ms, as the main would while offline: none of these ticks reads. */
static void tick_until(uint64_t probe_at_ms)
{
    /* Invariant: every period before now_ms has been ticked. */
    for (uint64_t now_ms = probe_at_ms - GT_HEALTH_RETRY_MS + GT_LIGHT_PERIOD_MS;
         now_ms < probe_at_ms; now_ms += GT_LIGHT_PERIOD_MS) {
        CHECK_EQ(gt_light_loop_tick(&rig.loop, now_ms, NULL), GT_OK);
    }
}

/* ---- LLP-05: the 10th failure logs sensor_offline once ----
 * Ten due ticks fail in a row (0..9000 ms). The 10th logs sensor_offline
 * at WARN, after its own i2c_read_error. While offline, the due ticks at
 * 10000..13000 don't touch the bus; the probe at 14000 (5 s after going
 * offline) reads again, fails, logs one more i2c_read_error and no second
 * sensor_offline. */
static void test_llp05_tenth_failure_goes_offline(void)
{
    start_rig(0);
    const uint64_t probe_at_ms = go_offline();   /* 9000 + 5000 */
    CHECK_EQ(logged(&rig.book, GT_LOG_WARN, GT_HEALTH_I2C_READ_ERROR), GT_HEALTH_OFFLINE_AFTER);
    CHECK_EQ(logged(&rig.book, GT_LOG_WARN, "sensor_offline"), 1);
    CHECK_EQ(rig.loop.health.state, GT_HEALTH_OFFLINE);

    int reads_when_offline = rig.bus.read_calls;
    tick_until(probe_at_ms);
    CHECK_EQ(rig.bus.read_calls, reads_when_offline);

    CHECK_EQ(gt_light_loop_tick(&rig.loop, probe_at_ms, NULL), GT_OK);
    CHECK(rig.bus.read_calls > reads_when_offline);
    CHECK_EQ(logged(&rig.book, GT_LOG_WARN, GT_HEALTH_I2C_READ_ERROR), GT_HEALTH_OFFLINE_AFTER + 1);
    CHECK_EQ(logged(&rig.book, GT_LOG_WARN, "sensor_offline"), 1);
    CHECK_EQ(rig.hub.n_sent, 0);

    stop_rig();
}

/* ---- LLP-06: a good probe reinits, then marks online ----
 * Offline, the probe's read succeeds, but a replugged chip has lost its
 * mode, so that first value (0) is stale. The loop re-runs the chip's init
 * (3 more commands after open's 3), logs sensor_reinit at INFO, reads
 * again, and only that fresh value (500) is sent, as counter 0. Then it
 * logs sensor_online at INFO, after sensor_reinit. */
static void test_llp06_probe_reinits_then_online(void)
{
    start_rig(0);
    const uint64_t probe_at_ms = go_offline();
    tick_until(probe_at_ms);
    push_reading(0);     /* the stale probe read */
    push_reading(500);   /* the fresh read after reinit */

    CHECK_EQ(gt_light_loop_tick(&rig.loop, probe_at_ms, NULL), GT_OK);
    CHECK_EQ(rig.bus.write_calls, 3 + 3);
    CHECK_EQ(rig.loop.health.state, GT_HEALTH_ONLINE);
    CHECK_EQ(rig.hub.n_sent, 1);
    CHECK_EQ(sent_message(0).value, 500);
    CHECK_EQ(sent_message(0).counter, 0);
    CHECK_EQ(logged(&rig.book, GT_LOG_INFO, "sensor_reinit"), 1);
    CHECK_EQ(logged(&rig.book, GT_LOG_INFO, "sensor_online"), 1);
    CHECK(rig.book.count >= 2);
    CHECK(strcmp(rig.book.lines[rig.book.count - 2].event, "sensor_reinit") == 0);
    CHECK(strcmp(rig.book.lines[rig.book.count - 1].event, "sensor_online") == 0);

    stop_rig();
}

/* ---- LLP-07: a failed reinit stays offline ----
 * Offline, the probe's read succeeds but the chip NACKs the first reinit
 * command (still half-plugged). The loop doesn't read again, sends
 * nothing, and counts it as a failed probe: one i2c_read_error at WARN, no
 * sensor_reinit (logged only when the init went through), no
 * sensor_online, and the next probe 5 s later. */
static void test_llp07_failed_reinit_stays_offline(void)
{
    start_rig(0);
    const uint64_t probe_at_ms = go_offline();
    tick_until(probe_at_ms);
    push_reading(0);
    rig.bus.fail_write_at = rig.bus.write_calls + 1;   /* reinit's POWER_ON */
    int reads_before = rig.bus.read_calls;

    CHECK_EQ(gt_light_loop_tick(&rig.loop, probe_at_ms, NULL), GT_OK);
    CHECK_EQ(rig.bus.read_calls, reads_before + 1);
    CHECK_EQ(rig.loop.health.state, GT_HEALTH_OFFLINE);
    CHECK_EQ(rig.loop.health.next_probe_ms, probe_at_ms + GT_HEALTH_RETRY_MS);
    CHECK_EQ(rig.hub.n_sent, 0);
    CHECK_EQ(logged(&rig.book, GT_LOG_WARN, GT_HEALTH_I2C_READ_ERROR), GT_HEALTH_OFFLINE_AFTER + 1);
    CHECK_EQ(logged(&rig.book, GT_LOG_INFO, "sensor_reinit"), 0);
    CHECK_EQ(logged(&rig.book, GT_LOG_INFO, "sensor_online"), 0);

    stop_rig();
}

/* ---- LLP-08: 0xFFFF is sent and logs light_saturated ----
 * Direct sun clips the BH1750 at 0xFFFF. That is real data, so it is sent
 * like any reading (counter 0, value 65535), and the tick also logs one
 * light_saturated at INFO to flag it as clipped. A normal reading on the
 * next tick logs nothing more. */
static void test_llp08_saturated_sent_and_logged(void)
{
    start_rig(0);
    push_reading(0xFFFF);
    CHECK_EQ(gt_light_loop_tick(&rig.loop, 0, NULL), GT_OK);
    CHECK_EQ(rig.hub.n_sent, 1);
    CHECK_EQ(sent_message(0).value, 0xFFFF);
    CHECK_EQ(sent_message(0).counter, 0);
    CHECK_EQ(rig.book.count, 1);
    CHECK_EQ(logged(&rig.book, GT_LOG_INFO, "light_saturated"), 1);

    push_reading(300);
    CHECK_EQ(gt_light_loop_tick(&rig.loop, GT_LIGHT_PERIOD_MS, NULL), GT_OK);
    CHECK_EQ(rig.hub.n_sent, 2);
    CHECK_EQ(rig.book.count, 1);

    stop_rig();
}

/* ---- LLP-09: overrun skips slots and keeps the anchor ----
 * The reading due at 1000 ms is only ticked at 3500 (a slow bus or a
 * stalled process). That tick reads once, not three times in a burst: the
 * slots at 2000 and 3000 are skipped and logged as one sample_overrun at
 * WARN with skipped=2. The next reading is due at 4000, still on the
 * 1000 ms grid from start, not at 3500 + 1000. */
static void test_llp09_overrun_keeps_anchor(void)
{
    start_rig(0);
    push_reading(100);
    push_reading(200);
    CHECK_EQ(gt_light_loop_tick(&rig.loop, 0, NULL), GT_OK);
    int reads_before = rig.bus.read_calls;

    uint64_t next_due = 0;
    CHECK_EQ(gt_light_loop_tick(&rig.loop, 3500, &next_due), GT_OK);
    CHECK_EQ(rig.bus.read_calls, reads_before + 1);
    CHECK_EQ(rig.hub.n_sent, 2);
    CHECK_EQ(next_due, 4 * GT_LIGHT_PERIOD_MS);
    CHECK_EQ(logged(&rig.book, GT_LOG_WARN, "sample_overrun"), 1);
    const char *fields = logged_fields(&rig.book, "sample_overrun");
    CHECK(fields != NULL && strcmp(fields, "skipped=2") == 0);

    stop_rig();
}

/* ---- LLP-10: hub down: one disconnect, one reconnect ----
 * The first reading goes out. The hub then dies: sending the second fails
 * (EPIPE), so the tick logs ipc_disconnect at WARN and the reading waits
 * in the ring. By the third tick the retry is due, so the client
 * reconnects (ipc_reconnect at INFO) and the hub gets both waiting
 * readings in order: counters 0, 1, 2 with no hole. Each event is logged
 * once. */
static void test_llp10_hub_down_logs_ipc_events(void)
{
    start_rig(0);
    fake_sock_push_send_result(&rig.hub, GT_OK);
    fake_sock_push_send_result(&rig.hub, GT_ERR_IO);
    const uint32_t values[3] = { 10, 20, 30 };
    /* Invariant: ticks 0..tick_index-1 each read one value and handed it to the client. */
    for (size_t tick_index = 0; tick_index < 3; tick_index++) {
        push_reading(values[tick_index]);
        CHECK_EQ(gt_light_loop_tick(&rig.loop, tick_index * GT_LIGHT_PERIOD_MS, NULL), GT_OK);
    }

    CHECK_EQ(logged(&rig.book, GT_LOG_WARN, "ipc_disconnect"), 1);
    CHECK_EQ(logged(&rig.book, GT_LOG_INFO, "ipc_reconnect"), 1);
    CHECK_EQ(rig.book.count, 2);
    CHECK_EQ(rig.hub.n_sent, 3);
    /* Invariant: messages 0..sent_index-1 arrived in counter order. */
    for (size_t sent_index = 0; sent_index < 3; sent_index++) {
        CHECK_EQ(sent_message(sent_index).counter, sent_index);
        CHECK_EQ(sent_message(sent_index).value, values[sent_index]);
    }

    stop_rig();
}

/* ---- LLP-11: a full ring logs ipc_overflow refused=N ----
 * The hub stays down for 66 readings. The first 64 fill the client's
 * ring; the 65th is refused, and that tick logs one ipc_overflow at WARN
 * with refused=1. The 66th is refused too but logs nothing new (one line
 * per overflow spell). Refused readings still use a counter, so the
 * counter is 66. When the hub comes back, it gets counters 0..63, then a
 * GAP starting at counter 64 covering 2 readings, then counter 66: every
 * counter is accounted for. */
static void test_llp11_full_ring_logs_overflow(void)
{
    start_rig(0);
    rig.hub.fail_connects = 1000000;
    const size_t readings_while_down = GT_IPC_RING_SLOTS + 2;
    uint64_t now_ms = 0;
    /* Invariant: ticks 0..reading_index-1 each handed one reading to the client. */
    for (size_t reading_index = 0; reading_index < readings_while_down; reading_index++) {
        now_ms = reading_index * GT_LIGHT_PERIOD_MS;
        push_reading((uint32_t)reading_index);
        CHECK_EQ(gt_light_loop_tick(&rig.loop, now_ms, NULL), GT_OK);
    }
    CHECK_EQ(logged(&rig.book, GT_LOG_WARN, "ipc_overflow"), 1);
    const char *fields = logged_fields(&rig.book, "ipc_overflow");
    CHECK(fields != NULL && strcmp(fields, "refused=1") == 0);
    CHECK_EQ(rig.loop.counter, readings_while_down);
    CHECK_EQ(rig.hub.n_sent, 0);

    rig.hub.fail_connects = 0;
    push_reading(500);
    CHECK_EQ(gt_light_loop_tick(&rig.loop, now_ms + GT_LIGHT_PERIOD_MS, NULL), GT_OK);
    CHECK_EQ(rig.hub.n_sent, GT_IPC_RING_SLOTS + 2);
    CHECK_EQ(sent_message(GT_IPC_RING_SLOTS - 1).counter, GT_IPC_RING_SLOTS - 1);
    gt_msg gap = sent_message(GT_IPC_RING_SLOTS);
    CHECK_EQ(gap.type, GT_MSG_GAP);
    CHECK_EQ(gap.counter, GT_IPC_RING_SLOTS);
    CHECK_EQ(gap.value, 2);
    CHECK_EQ(sent_message(GT_IPC_RING_SLOTS + 1).counter, readings_while_down);
    CHECK_EQ(sent_message(GT_IPC_RING_SLOTS + 1).value, 500);

    stop_rig();
}

/* Expect gt_light_loop_init to reject `ctx` with GT_ERR_INVAL and leave
 * the loop's bytes exactly as they were. memcmp, not ==, because C can't
 * compare structs with ==, and copying one with = needn't copy padding. */
static void check_init_rejects(const gt_light_loop_ctx *ctx)
{
    gt_light_loop target;
    gt_light_loop before;
    memset(&target, 0xAB, sizeof target);
    memcpy(&before, &target, sizeof target);
    CHECK_EQ(gt_light_loop_init(&target, ctx, 0), GT_ERR_INVAL);
    CHECK(memcmp(&target, &before, sizeof target) == 0);
}

/* ---- LLP-12: NULL args give GT_ERR_INVAL ----
 * init rejects a NULL loop or ctx, and a ctx missing its sensor, client
 * or log callback, leaving the loop untouched. tick rejects a NULL loop
 * and leaves *next_due_out untouched. The loop never dereferences NULL. */
static void test_llp12_null_args_rejected(void)
{
    start_rig(0);
    const gt_light_loop_ctx good = rig.loop.ctx;
    CHECK_EQ(gt_light_loop_init(NULL, &good, 0), GT_ERR_INVAL);
    check_init_rejects(NULL);

    gt_light_loop_ctx missing = good;
    missing.sensor = NULL;
    check_init_rejects(&missing);
    missing = good;
    missing.client = NULL;
    check_init_rejects(&missing);
    missing = good;
    missing.log = NULL;
    check_init_rejects(&missing);

    uint64_t next_due = 0xDEADBEEFu;
    CHECK_EQ(gt_light_loop_tick(NULL, 0, &next_due), GT_ERR_INVAL);
    CHECK_EQ(next_due, 0xDEADBEEFu);

    stop_rig();
}

static const test_case_t tests[] = {
    {"LLP-01", "a due tick reads and sends DATA counter 0", test_llp01_due_tick_sends_data},
    {"LLP-02", "the counter goes up one per reading", test_llp02_counter_counts_up},
    {"LLP-03", "a tick before it's due doesn't read but pumps", test_llp03_not_due_still_pumps},
    {"LLP-04", "a failed read logs i2c_read_error", test_llp04_failed_read_logs_error},
    {"LLP-05", "the 10th failure logs sensor_offline once", test_llp05_tenth_failure_goes_offline},
    {"LLP-06", "a good probe reinits, then marks online", test_llp06_probe_reinits_then_online},
    {"LLP-07", "a failed reinit stays offline", test_llp07_failed_reinit_stays_offline},
    {"LLP-08", "0xFFFF is sent and logs light_saturated", test_llp08_saturated_sent_and_logged},
    {"LLP-09", "overrun skips slots and keeps the anchor", test_llp09_overrun_keeps_anchor},
    {"LLP-10", "hub down: one disconnect, one reconnect", test_llp10_hub_down_logs_ipc_events},
    {"LLP-11", "a full ring logs ipc_overflow refused=N", test_llp11_full_ring_logs_overflow},
    {"LLP-12", "NULL args give GT_ERR_INVAL", test_llp12_null_args_rejected},
};

int main(void)
{
    return RUN_TESTS(tests);
}
