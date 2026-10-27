/* The storage process's flush thread (see storage_server.h).
 *
 * Purpose: once a second (or at once when an append makes a flush due),
 * run one tick: log clock sync changes, roll over to a new day's segment
 * when the clock allows it, flush if due, and turn what storage reports
 * into log lines. Every decision is made by storage; this file only
 * decides when to ask and how often to log the same problem. The rx loop
 * on storage.sock is the other half of the process, in storage_server.c. */
#include "storage_server.h"

#include <stdio.h>
#include <time.h>

#define FLUSHER_PERIOD_MS 1000
#define FLUSHER_STEP_MS   100   /* how often the sleeping flush thread checks stop */

/* Monotonic time in ms (never jumps when the wall clock is set), the clock
 * every append and flush_due uses. */
static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Wall-clock time in us since the epoch: picks the day's segment. */
static uint64_t wall_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

void gt_flusher_init(gt_flusher *flusher, gt_storage *storage, unsigned retention_days,
                     gt_storage_log_fn log, void *log_ctx)
{
    if (flusher == NULL) {
        return;
    }
    flusher->storage = storage;
    flusher->retention_days = retention_days;
    flusher->log = log;
    flusher->log_ctx = log_ctx;
    flusher->logged_unsynced = 0;
    flusher->logged_create_error = 0;
    flusher->clock_unsynced = 0;
    flusher->unsynced_since_ms = 0;
}

/* Records keep flowing while the clock is unsynced (only rollover and
 * retention wait), so the flush runs either way. */
void gt_storage_log_commit(gt_storage_log_fn log, void *log_ctx, const gt_storage *storage,
                           uint64_t commits_before)
{
    gt_storage_stats stats;
    if (log == NULL || gt_storage_read_stats(storage, &stats) != GT_OK
        || stats.commits == commits_before) {
        return;
    }
    char fields[96];
    snprintf(fields, sizeof fields, "records=%lu commit_us=%llu fsync_us=%llu",
             (unsigned long)stats.last_commit_records,
             (unsigned long long)stats.last_commit_us, (unsigned long long)stats.last_fsync_us);
    log(log_ctx, GT_LOG_INFO, "storage_commit", fields);
    if (stats.last_fsync_us > GT_FSYNC_SLOW_US) {
        snprintf(fields, sizeof fields, "fsync_us=%llu records=%lu",
                 (unsigned long long)stats.last_fsync_us,
                 (unsigned long)stats.last_commit_records);
        log(log_ctx, GT_LOG_WARN, "fsync_slow", fields);
    }
}

/* Log the start of an unsynced spell (clock_unsynced) and its end with its
 * length (clock_synced unsynced_ms=N), once each (PF-13). */
static void log_clock_change(gt_flusher *flusher, uint64_t now_ms, int clock_synced)
{
    if (!clock_synced && !flusher->clock_unsynced) {
        flusher->clock_unsynced = 1;
        flusher->unsynced_since_ms = now_ms;
        if (flusher->log != NULL) {
            flusher->log(flusher->log_ctx, GT_LOG_WARN, "clock_unsynced", NULL);
        }
    } else if (clock_synced && flusher->clock_unsynced) {
        flusher->clock_unsynced = 0;
        if (flusher->log != NULL) {
            char fields[48];
            snprintf(fields, sizeof fields, "unsynced_ms=%llu",
                     (unsigned long long)(now_ms - flusher->unsynced_since_ms));
            flusher->log(flusher->log_ctx, GT_LOG_INFO, "clock_synced", fields);
        }
    }
}

/* Log a repeating problem once per spell: if `bit` is in events and was
 * already logged this spell (*logged), take it out of events. *logged is
 * set while the bit keeps coming, and cleared once `spell_over` says the
 * problem has ended, so the next one is logged again. Returns events. */
static unsigned suppress_repeat(unsigned events, unsigned bit, int spell_over, int *logged)
{
    if (!(events & bit)) {
        if (spell_over) {
            *logged = 0;
        }
        return events;
    }
    if (*logged) {
        events &= ~bit;   /* already logged this spell */
    }
    *logged = 1;
    return events;
}

/* A pass commits at most once: either the rollover's pre-flush (PF-06),
 * which leaves nothing pending, or the due flush after it. So comparing
 * the commit count before and after finds that one commit (PF-08). An
 * unsynced clock's retention skip is logged once per unsynced spell, and
 * a segment create error once until a rollover succeeds. */
void gt_flusher_tick(gt_flusher *flusher, uint64_t now_us, uint64_t now_ms, int clock_synced)
{
    if (flusher == NULL || flusher->storage == NULL) {
        return;
    }
    gt_storage_stats before;
    uint64_t commits_before = gt_storage_read_stats(flusher->storage, &before) == GT_OK
                                  ? before.commits : 0;
    log_clock_change(flusher, now_ms, clock_synced);
    unsigned events = 0;
    gt_storage_rollover(flusher->storage, now_us, clock_synced, flusher->retention_days,
                        &events);
    events = suppress_repeat(events, GT_STORAGE_EV_RETENTION_SKIPPED, clock_synced,
                             &flusher->logged_unsynced);
    events = suppress_repeat(events, GT_STORAGE_EV_SEGMENT_CREATE_ERROR,
                             (events & GT_STORAGE_EV_ROLLOVER) != 0,
                             &flusher->logged_create_error);
    gt_storage_log_events(flusher->log, flusher->log_ctx, events);

    if (gt_storage_flush_due(flusher->storage, now_ms)) {
        unsigned flush_events = 0;
        gt_storage_flush(flusher->storage, &flush_events);
        gt_storage_log_events(flusher->log, flusher->log_ctx, flush_events);
    }
    gt_storage_log_commit(flusher->log, flusher->log_ctx, flusher->storage, commits_before);
}

/* The sleep is cut into FLUSHER_STEP_MS pieces so a stop is noticed
 * within 0.1 s rather than a whole period. Each piece is a
 * gt_storage_wait_due rather than a plain sleep (PF-11): when an append
 * makes a flush due (N reached), or storage_main calls gt_storage_wake,
 * the wait returns at once and the next tick runs straight away, so
 * N = 1 means one fsync per record instead of one per second. The age
 * rule (T), rollover and the clock checks still run on the 1 s tick. */
void gt_flusher_run(gt_flusher *flusher, const atomic_int *stop, int (*clock_synced_fn)(void))
{
    if (flusher == NULL || stop == NULL) {
        return;
    }
    while (!atomic_load(stop)) {
        int synced = clock_synced_fn != NULL ? clock_synced_fn() : 1;
        gt_flusher_tick(flusher, wall_us(), monotonic_ms(), synced);
        /* invariant: slept_ms of this period have passed, with no stop
         * seen and no flush becoming due */
        for (int slept_ms = 0; slept_ms < FLUSHER_PERIOD_MS && !atomic_load(stop);
             slept_ms += FLUSHER_STEP_MS) {
            if (gt_storage_wait_due(flusher->storage, FLUSHER_STEP_MS)) {
                break;   /* a flush became due (or a wake): tick now */
            }
        }
    }
}
