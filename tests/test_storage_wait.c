/* Unit tests for waking the flush thread (src/storage.c):
 * gt_storage_wait_due and gt_storage_wake, with the signal raised by
 * gt_storage_append.
 *
 * Purpose: prove the flush thread can sleep until a flush becomes due
 * instead of looking once a second, so N = 1 really means one fsync per
 * record (PF-11): a signal raised before the wait is kept, a wait with
 * nothing due times out, each signal is used once (so a flush that stays
 * due after a failure can't make the flusher spin), and an append or a
 * wake on another thread ends a wait at once. Uses tests/fake_file.h and
 * real threads (pthread_create). Run under make test / asan / memcheck.
 * Implements SWT-01 through SWT-06.
 *
 * The two cross-thread tests check the wait ended well before its 2 s
 * timeout (under 1.5 s). The bound is loose so a slow run (Valgrind) can't
 * fail it, but a wait that ignored the wake would take the full 2 s. */
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TEST_CAPACITY 8
#define TEST_BATCH    2      /* N: a flush becomes due at 2 records */
#define LONG_WAIT_MS  2000u  /* long enough that only a wake ends it early */
#define SHORT_WAIT_MS 20u    /* a wait expected to time out */

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* Open storage on a fresh, empty fake directory with N = TEST_BATCH and
 * the default T, so only N can make a flush due in these tests. */
static gt_storage *open_storage(void)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    gt_storage *storage = NULL;
    gt_storage_open_args args = { .ops = ops, .ctx = &fs, .now_us = TODAY_US,
                                  .pending_capacity = TEST_CAPACITY };
    CHECK_EQ(gt_storage_open_ops(&args, &storage, ), );
    CHECK_EQ(gt_storage_set_batch(storage, TEST_BATCH, GT_BATCH_MAX_AGE_S), GT_OK);
    return storage;
}

/* Append light DATA records with seqs first_seq .. first_seq + count - 1,
 * all at 0 ms, checking each is accepted. */
static void append_records(gt_storage *storage, uint32_t first_seq, size_t count)
{
    for (size_t added = 0; added < count; added++) {
        const gt_record record = {
            .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
            .seq = first_seq + (uint32_t)added, .timestamp_us = 5000, .value = 100,
        };
        uint8_t wire[GT_REC_SIZE];
        CHECK_EQ(gt_record_encode(&record, wire), GT_OK);
        CHECK_EQ(gt_storage_append(storage, wire, 0, NULL), GT_OK);
    }
}

/* Monotonic time in ms, for timing a wait. */
static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* What a waiter thread was given and what it saw. */
typedef struct {
    gt_storage *storage;
    int         result;       /* gt_storage_wait_due's return */
    uint64_t    waited_ms;    /* how long that call took */
} waiter;

/* A waiter thread's body: one LONG_WAIT_MS wait, timed. pthread_create
 * runs it on a new thread with the waiter as `arg`; a thread function
 * must take and return void *, so the struct is passed through one. */
static void *waiter_main(void *arg)
{
    waiter *self = arg;
    uint64_t start_ms = monotonic_ms();
    self->result = gt_storage_wait_due(self->storage, LONG_WAIT_MS);
    self->waited_ms = monotonic_ms() - start_ms;
    return NULL;
}

/* Start a waiter on its own thread (`pthread_t` names a thread, the way
 * an fd names a file) and give it 50 ms to reach its wait. */
static void start_waiter(pthread_t *thread, waiter *self, gt_storage *storage)
{
    const struct timespec settle = { .tv_sec = 0, .tv_nsec = 50 * 1000000L };
    self->storage = storage;
    self->result = -1;
    self->waited_ms = 0;
    CHECK_EQ(pthread_create(thread, NULL, waiter_main, self), 0);
    nanosleep(&settle, NULL);
}

/* ---- SWT-01: a signal raised before the wait is kept ----
 * Two appends make a flush due before anyone waits; the wait still sees
 * it, so a flush can't be missed because the flusher was busy. */
static void test_swt01_early_signal_kept(void)
{
    gt_storage *storage = open_storage();

    append_records(storage, 1, TEST_BATCH);
    CHECK_EQ(gt_storage_wait_due(storage, LONG_WAIT_MS), 1);

    gt_storage_close(storage);
}

/* ---- SWT-02: nothing due: the wait times out ----
 * Empty, then one record (below N = 2): each wait returns 0. */
static void test_swt02_not_due_times_out(void)
{
    gt_storage *storage = open_storage();

    CHECK_EQ(gt_storage_wait_due(storage, SHORT_WAIT_MS), 0);
    append_records(storage, 1, 1);
    CHECK_EQ(gt_storage_wait_due(storage, SHORT_WAIT_MS), 0);

    gt_storage_close(storage);
}

/* ---- SWT-03: each signal is used once ----
 * Two records make a flush due and the first wait uses the signal. Nothing
 * is flushed, as after a failed write: the records stay pending and the
 * flush stays due, but the next wait sleeps (times out) rather than
 * returning at once. A third append while still due raises nothing new. */
static void test_swt03_signal_used_once(void)
{
    gt_storage *storage = open_storage();

    append_records(storage, 1, TEST_BATCH);
    CHECK_EQ(gt_storage_wait_due(storage, LONG_WAIT_MS), 1);
    CHECK_EQ(gt_storage_flush_due(storage, 0), 1);
    CHECK_EQ(gt_storage_wait_due(storage, SHORT_WAIT_MS), 0);
    append_records(storage, 1 + TEST_BATCH, 1);
    CHECK_EQ(gt_storage_wait_due(storage, SHORT_WAIT_MS), 0);

    gt_storage_close(storage);
}

/* ---- SWT-04: an append on another thread ends a wait ----
 * The flush thread's real case: it is waiting when the rx thread's append
 * makes a flush due. The wait returns 1 well before its 2 s timeout. */
static void test_swt04_append_wakes_waiter(void)
{
    gt_storage *storage = open_storage();
    pthread_t thread;
    waiter self;

    start_waiter(&thread, &self, storage);
    append_records(storage, 1, TEST_BATCH);
    CHECK_EQ(pthread_join(thread, NULL), 0);
    CHECK_EQ(self.result, 1);
    CHECK(self.waited_ms < 1500);

    gt_storage_close(storage);
}

/* ---- SWT-05: gt_storage_wake ends a wait ----
 * storage_main's stop: nothing is due, but the waiting flusher must
 * return at once so it can see the stop flag. The wake is used once too. */
static void test_swt05_wake_ends_wait(void)
{
    gt_storage *storage = open_storage();
    pthread_t thread;
    waiter self;

    start_waiter(&thread, &self, storage);
    gt_storage_wake(storage);
    CHECK_EQ(pthread_join(thread, NULL), 0);
    CHECK_EQ(self.result, 1);
    CHECK(self.waited_ms < 1500);
    CHECK_EQ(gt_storage_wait_due(storage, SHORT_WAIT_MS), 0);

    gt_storage_close(storage);
}

/* ---- SWT-06: NULL storage is safe ----
 * wait returns 0 at once; wake does nothing. */
static void test_swt06_null_storage(void)
{
    CHECK_EQ(gt_storage_wait_due(NULL, LONG_WAIT_MS), 0);
    gt_storage_wake(NULL);
}

static const test_case_t tests[] = {
    {"SWT-01", "a signal raised before the wait is kept", test_swt01_early_signal_kept},
    {"SWT-02", "nothing due: the wait times out", test_swt02_not_due_times_out},
    {"SWT-03", "each signal is used once", test_swt03_signal_used_once},
    {"SWT-04", "an append on another thread ends a wait", test_swt04_append_wakes_waiter},
    {"SWT-05", "gt_storage_wake ends a wait", test_swt05_wake_ends_wait},
    {"SWT-06", "NULL storage is safe", test_swt06_null_storage},
};

int main(void)
{
    return RUN_TESTS(tests);
}
