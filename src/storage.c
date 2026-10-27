/* Storage: the one component that writes the log (see storage.h).
 *
 * Purpose: the rx thread's side of the storage object: append (check the
 * seq, then queue for the next flush), the rx frame handler and the
 * seq_resume reply, the "flush is due" signal the flush thread sleeps on,
 * and the settings and stats both threads share.
 *
 * The rest of the storage object lives beside it, sharing the struct
 * through storage_internal.h:
 *   - storage_segment.c: segment names and the sorted segment listing;
 *   - storage_open.c:    open (marker, recovery, active segment) and close;
 *   - storage_flush.c:   flush, rollover + retention, and shutdown. */
#include "storage_internal.h"

#include <string.h>

#include "storage_seq.h"

/* The highest backlog threshold `pending` records have reached: 90, 75,
 * 50 or 0 (DESIGN.md "storage_backlog at 50/75/90%"). Integer-only:
 * pending * 100 >= capacity * threshold. Widened to uint64_t so the
 * multiplication can't overflow even where size_t is 32 bits. Declared in
 * storage_internal.h: flush uses it too. */
unsigned gt_storage_backlog_level(size_t pending, size_t capacity)
{
    static const unsigned thresholds[] = { 90, 75, 50 };
    uint64_t scaled_pending = (uint64_t)pending * 100u;

    for (size_t threshold_index = 0; threshold_index < 3; threshold_index++) {
        if (scaled_pending >= (uint64_t)capacity * thresholds[threshold_index]) {
            return thresholds[threshold_index];
        }
    }
    return 0;
}

/* A reading refused because the buffer is full: count it, and report the
 * first refusal of a full episode once. */
static gt_status refuse_full(gt_storage *storage, unsigned *events)
{
    storage->refused++;
    if (!storage->full_episode) {
        storage->full_episode = 1;
        *events |= GT_STORAGE_EV_QUEUE_FULL;
    }
    return GT_ERR_FULL;
}

/* The part of append that runs under the lock: judge the seq, then store
 * the record (and its GAP first, if seqs are missing). A GAP needs two
 * free slots, checked before either is added, so it is both or neither.
 * The seq check comes before any room check, so a duplicate arriving
 * while full is rejected as a duplicate, never counted as refused. */
static gt_status append_locked(gt_storage *storage, const uint8_t record[GT_REC_SIZE],
                               const gt_record *decoded, uint64_t now_ms, unsigned *events)
{
    gt_seq_verdict verdict = gt_seq_check(storage->last_accepted_seq,
                                          storage->has_last_accepted, decoded->seq);
    if (verdict == GT_SEQ_REJECT) {
        *events |= GT_STORAGE_EV_SEQ_REJECTED;
        return GT_ERR_CORRUPT;
    }

    size_t room = storage->capacity - gt_pending_count(storage->pending);
    if (room < (verdict == GT_SEQ_GAP ? 2u : 1u)) {
        return refuse_full(storage, events);
    }

    if (verdict == GT_SEQ_GAP) {
        gt_record gap;
        uint8_t gap_wire[GT_REC_SIZE];
        if (gt_seq_gap_record(storage->last_accepted_seq, storage->last_accepted_us, decoded,
                              &gap) != GT_OK
            || gt_record_encode(&gap, gap_wire) != GT_OK
            || gt_pending_add(storage->pending, gap_wire, now_ms) != GT_OK) {
            return refuse_full(storage, events);   /* can't happen after the room check */
        }
        *events |= GT_STORAGE_EV_RECOVERY_GAP;
    }
    if (gt_pending_add(storage->pending, record, now_ms) != GT_OK) {
        return refuse_full(storage, events);       /* can't happen after the room check */
    }

    storage->has_last_accepted = 1;
    storage->last_accepted_seq = decoded->seq;
    storage->last_accepted_us = decoded->timestamp_us;
    storage->full_episode = 0;

    unsigned level = gt_storage_backlog_level(gt_pending_count(storage->pending),
                                              storage->capacity);
    if (level > storage->backlog_percent) {
        *events |= GT_STORAGE_EV_BACKLOG;
    }
    storage->backlog_percent = level;
    return GT_OK;
}

/* Decoding needs no lock (it only reads `record`), so it runs first and a
 * bad record never takes the lock at all. */
gt_status gt_storage_append(gt_storage *storage, const uint8_t record[GT_REC_SIZE],
                            uint64_t now_ms, unsigned *events_out)
{
    unsigned events = 0;
    gt_status status;
    gt_record decoded;

    if (storage == NULL || record == NULL) {
        status = GT_ERR_INVAL;
    } else if (gt_record_decode(record, &decoded) != GT_OK) {
        status = GT_ERR_CORRUPT;
    } else {
        pthread_mutex_lock(&storage->lock);
        int was_due = gt_pending_due(storage->pending, now_ms);
        status = append_locked(storage, record, &decoded, now_ms, &events);
        /* Signal only the change to due, not every append while due (see
         * gt_storage_wait_due in storage.h): a waiting flusher wakes now. */
        if (!was_due && gt_pending_due(storage->pending, now_ms)) {
            storage->due_signal = 1;
            pthread_cond_signal(&storage->due_cond);
        }
        pthread_mutex_unlock(&storage->lock);
    }
    if (events_out != NULL) {
        *events_out = events;
    }
    return status;
}

int gt_storage_flush_due(gt_storage *storage, uint64_t now_ms)
{
    if (storage == NULL) {
        return 0;
    }
    pthread_mutex_lock(&storage->lock);
    int due = gt_pending_due(storage->pending, now_ms);
    pthread_mutex_unlock(&storage->lock);
    return due;
}

/* pthread_cond_timedwait takes an absolute deadline on WAIT_CLOCK, not a
 * duration, so a wait that wakes early for no reason (allowed by POSIX)
 * and goes back to sleep still ends on time. It lets go of the lock while
 * asleep and takes it back before returning. */
int gt_storage_wait_due(gt_storage *storage, unsigned max_wait_ms)
{
    if (storage == NULL) {
        return 0;
    }
    struct timespec deadline;
    clock_gettime(WAIT_CLOCK, &deadline);
    deadline.tv_sec += max_wait_ms / 1000u;
    deadline.tv_nsec += (long)(max_wait_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&storage->lock);
    /* invariant: the signal was not raised at any check so far, and every
     * wake so far came before the deadline */
    while (!storage->due_signal) {
        if (pthread_cond_timedwait(&storage->due_cond, &storage->lock, &deadline) != 0) {
            break;   /* ETIMEDOUT (or an error): stop waiting */
        }
    }
    int raised = storage->due_signal;
    storage->due_signal = 0;   /* used up: the next wait sleeps again */
    pthread_mutex_unlock(&storage->lock);
    return raised;
}

void gt_storage_wake(gt_storage *storage)
{
    if (storage == NULL) {
        return;
    }
    pthread_mutex_lock(&storage->lock);
    storage->due_signal = 1;
    pthread_cond_signal(&storage->due_cond);
    pthread_mutex_unlock(&storage->lock);
}

/* Everything but a malformed frame or a disconnect keeps the connection:
 * a refused record (old seq, full buffer) is storage's decision, not a
 * sign the hub is broken, so closing would only force a needless
 * reconnect. A record's bad bytes are the one append refusal that means
 * the stream can't be trusted. */
gt_rx_action gt_storage_rx(gt_storage *storage, const uint8_t *buf, long len,
                           uint64_t now_ms, unsigned *events_out)
{
    unsigned events = 0;
    gt_rx_action action = GT_RX_CLOSE;

    if (storage != NULL && buf != NULL && len > 0) {
        if (len != GT_REC_SIZE) {
            events |= GT_STORAGE_EV_BAD_MESSAGE;
        } else {
            gt_status status = gt_storage_append(storage, buf, now_ms, &events);
            if (status == GT_ERR_CORRUPT && !(events & GT_STORAGE_EV_SEQ_REJECTED)) {
                events |= GT_STORAGE_EV_BAD_MESSAGE;   /* bad bytes */
            } else {
                action = GT_RX_KEEP;
            }
        }
    }
    if (events_out != NULL) {
        *events_out = events;
    }
    return action;
}

/* The same lock-through-const cast as gt_storage_read_stats. */
gt_status gt_storage_resume(const gt_storage *storage, gt_resume *resume_out)
{
    if (storage == NULL || resume_out == NULL) {
        return GT_ERR_INVAL;
    }
    pthread_mutex_t *lock = (pthread_mutex_t *)&storage->lock;
    pthread_mutex_lock(lock);
    resume_out->has_last = storage->has_last_accepted;
    resume_out->seq = storage->has_last_accepted ? storage->last_accepted_seq : 0;
    pthread_mutex_unlock(lock);
    return GT_OK;
}

/* No lock: active_name belongs to the flushing thread (see storage.h). */
gt_status gt_storage_active_segment(const gt_storage *storage, char name_out[GT_FILE_NAME_MAX])
{
    if (storage == NULL || name_out == NULL) {
        return GT_ERR_INVAL;
    }
    memcpy(name_out, storage->active_name, GT_FILE_NAME_MAX);
    return GT_OK;
}

/* No lock: this is called before the flush thread starts (see storage.h),
 * and only the flushing thread reads clock_us. */
void gt_storage_set_clock(gt_storage *storage, gt_clock_us_fn clock_us)
{
    if (storage == NULL) {
        return;
    }
    storage->clock_us = clock_us;
}

/* The limits live in the pending buffer, which checks them; storage only
 * converts T to ms and holds the lock, since gt_storage_flush_due reads
 * them under it. */
gt_status gt_storage_set_batch(gt_storage *storage, size_t batch_records, unsigned max_age_s)
{
    if (storage == NULL) {
        return GT_ERR_INVAL;
    }
    pthread_mutex_lock(&storage->lock);
    gt_status status = gt_pending_set_limits(storage->pending, batch_records,
                                             (uint64_t)max_age_s * 1000u);
    pthread_mutex_unlock(&storage->lock);
    return status;
}

/* stats_out is filled under the lock so its fields all come from the same
 * moment. The lock is changed by locking, so the `const` is cast away for
 * that one call; the storage itself was never defined const, so this is
 * allowed. */
gt_status gt_storage_read_stats(const gt_storage *storage, gt_storage_stats *stats_out)
{
    if (storage == NULL || stats_out == NULL) {
        return GT_ERR_INVAL;
    }
    pthread_mutex_t *lock = (pthread_mutex_t *)&storage->lock;
    pthread_mutex_lock(lock);
    stats_out->pending = gt_pending_count(storage->pending);
    stats_out->committed_records = storage->committed_records;
    stats_out->has_last_accepted = storage->has_last_accepted;
    stats_out->last_accepted_seq = storage->last_accepted_seq;
    stats_out->has_last_committed = storage->has_last_committed;
    stats_out->last_committed_seq = storage->last_committed_seq;
    stats_out->degraded = storage->degraded;
    stats_out->backlog_percent = storage->backlog_percent;
    stats_out->refused = storage->refused;
    stats_out->segments_deleted = storage->segments_deleted;
    stats_out->commits = storage->commits;
    stats_out->last_commit_records = storage->last_commit_records;
    stats_out->last_commit_us = storage->last_commit_us;
    stats_out->last_fsync_us = storage->last_fsync_us;
    pthread_mutex_unlock(lock);
    return GT_OK;
}
