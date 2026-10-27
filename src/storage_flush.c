/* The flushing thread's side of storage (see storage.h).
 *
 * Purpose: everything that puts records on the card and moves the log
 * forward: flush (write + fsync, the durability point, with the cut back
 * to the committed offset after a failure), rollover (a new segment each
 * UTC day, plus retention deleting the oldest), and shutdown (a final
 * flush, then the clean-shutdown marker).
 *
 * Only one thread ever calls these, so handle, active_name and
 * committed_offset are read and changed here without the lock; everything
 * append also touches is changed under it.
 *
 * All file access goes through the gt_file_ops seam (storage_io.h), so
 * tests run on tests/fake_file.h. */
#include "storage_internal.h"

#include <string.h>

/* The storage clock in us, or 0 when none was set. */
static uint64_t read_clock(const gt_storage *storage)
{
    return storage->clock_us != NULL ? storage->clock_us() : 0;
}

/* Step 2 of flush: put byte_count bytes of the flush buffer on the card.
 * After a failed flush (degraded), the file may end in torn bytes past
 * the committed offset, or hold a full copy that was never fsynced, so
 * it is first cut back to the committed offset. Writing on top of torn
 * bytes is never allowed: if the cut fails, nothing is written. Returns
 * GT_OK, or GT_ERR_IO with WRITE_ERROR or FSYNC_ERROR added to *events.
 * On GT_OK, *commit_us_out is the whole step's time and *fsync_us_out the
 * fsync's (PF-08); the clock is read before the cut/write, before the
 * fsync and after it. */
static gt_status write_batch(gt_storage *storage, size_t byte_count, unsigned *events,
                             uint64_t *commit_us_out, uint64_t *fsync_us_out)
{
    const gt_file_ops *ops = storage->ops;
    uint64_t start_us = read_clock(storage);

    if (storage->degraded
        && ops->ftruncate(storage->ctx, storage->handle, storage->committed_offset) != GT_OK) {
        *events |= GT_STORAGE_EV_WRITE_ERROR;
        return GT_ERR_IO;
    }
    if (ops->pwrite(storage->ctx, storage->handle, storage->committed_offset,
                    storage->flush_buf, byte_count) != GT_OK) {
        *events |= GT_STORAGE_EV_WRITE_ERROR;
        return GT_ERR_IO;
    }
    uint64_t fsync_start_us = read_clock(storage);
    if (ops->fsync(storage->ctx, storage->handle) != GT_OK) {
        *events |= GT_STORAGE_EV_FSYNC_ERROR;
        return GT_ERR_IO;
    }
    uint64_t end_us = read_clock(storage);
    *commit_us_out = end_us - start_us;
    *fsync_us_out = end_us - fsync_start_us;
    return GT_OK;
}

/* handle, committed_offset and degraded are read outside the lock: only
 * the one flushing thread ever changes them (see storage.h), and it
 * changes degraded under the lock so read_stats sees a consistent value.
 * Everything append touches is changed under the lock. */
gt_status gt_storage_flush(gt_storage *storage, unsigned *events_out)
{
    unsigned events = 0;
    if (events_out != NULL) {
        *events_out = 0;
    }
    if (storage == NULL) {
        return GT_ERR_INVAL;
    }

    /* 1. Copy the pending records out under the lock. */
    size_t record_count = 0;
    pthread_mutex_lock(&storage->lock);
    const uint8_t *pending_bytes = gt_pending_peek(storage->pending, &record_count);
    if (record_count > 0) {
        memcpy(storage->flush_buf, pending_bytes, record_count * GT_REC_SIZE);
    }
    pthread_mutex_unlock(&storage->lock);
    if (record_count == 0) {
        return GT_OK;
    }

    /* 2. Write and fsync with no lock held, so append never waits on the card. */
    size_t byte_count = record_count * GT_REC_SIZE;
    uint64_t commit_us = 0, fsync_us = 0;
    if (write_batch(storage, byte_count, &events, &commit_us, &fsync_us) != GT_OK) {
        /* The records are still pending; mark degraded (reported once). */
        pthread_mutex_lock(&storage->lock);
        if (!storage->degraded) {
            storage->degraded = 1;
            events |= GT_STORAGE_EV_DEGRADED;
        }
        pthread_mutex_unlock(&storage->lock);
        if (events_out != NULL) {
            *events_out = events;
        }
        return GT_ERR_IO;
    }

    /* 3. They are committed: drop them and move the counters on. Every
     * pending record was checked by append, so the last one decodes. */
    gt_record last_record;
    gt_status decoded = gt_record_decode(&storage->flush_buf[byte_count - GT_REC_SIZE],
                                         &last_record);
    pthread_mutex_lock(&storage->lock);
    gt_pending_commit(storage->pending, record_count);
    /* A commit only removes records, so the level can only go down, and
     * going down is silent; it re-arms the thresholds for the next rise. */
    storage->backlog_percent = gt_storage_backlog_level(gt_pending_count(storage->pending),
                                                        storage->capacity);
    storage->committed_offset += byte_count;
    storage->committed_records += record_count;
    storage->commits++;
    storage->last_commit_records = record_count;
    storage->last_commit_us = commit_us;
    storage->last_fsync_us = fsync_us;
    if (decoded == GT_OK) {
        storage->has_last_committed = 1;
        storage->last_committed_seq = last_record.seq;
    }
    if (storage->degraded) {
        storage->degraded = 0;
        events |= GT_STORAGE_EV_RECOVERED;
    }
    pthread_mutex_unlock(&storage->lock);
    if (events_out != NULL) {
        *events_out = events;
    }
    return GT_OK;
}

/* Step 3 of rollover: delete segments that are both before the window
 * (older than `cutoff`, the name of the window's first day) and not among
 * the newest retention_days segments. The active segment is never one of
 * them, but it is checked anyway. A failed delete is reported and the
 * others still go. */
static void delete_old_segments(gt_storage *storage, const char *cutoff,
                                unsigned retention_days, unsigned *events)
{
    char segments[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX];
    size_t segment_count = 0;
    int marker_present = 0;   /* not used here: the marker is never deleted by retention */
    if (gt_storage_list_segments(storage->ops, storage->ctx, segments, &segment_count,
                      &marker_present) != GT_OK) {
        *events |= GT_STORAGE_EV_RETENTION_ERROR;
        return;
    }
    if (segment_count <= retention_days) {
        return;   /* every segment is among the newest N */
    }

    /* Sorted oldest first, so the newest N are the last N: only
     * segments[0 .. segment_count - retention_days - 1] may go.
     * invariant: every candidate before old_index has been deleted or
     * reported */
    for (size_t old_index = 0; old_index < segment_count - retention_days; old_index++) {
        const char *name = segments[old_index];
        if (strcmp(name, cutoff) >= 0 || strcmp(name, storage->active_name) == 0) {
            continue;
        }
        if (storage->ops->unlink(storage->ctx, name) != GT_OK) {
            *events |= GT_STORAGE_EV_RETENTION_ERROR;
            continue;
        }
        pthread_mutex_lock(&storage->lock);
        storage->segments_deleted++;
        pthread_mutex_unlock(&storage->lock);
    }
}

/* Steps 1 and 2 of rollover, once the UTC day has moved past the active
 * segment's: finish the old segment, make `today` the active one, then
 * apply retention. Events are added to *events. */
static gt_status switch_segment(gt_storage *storage, const char *today, uint64_t now_us,
                                unsigned retention_days, unsigned *events)
{
    /* Records received before midnight belong in the old day's segment
     * (PF-06), so flush them there first. If this flush fails, storage
     * is degraded and the tail cut below runs as usual; the records stay
     * pending and go into the new segment. */
    unsigned flush_events = 0;
    gt_storage_flush(storage, &flush_events);
    *events |= flush_events;
    if (storage->degraded
        && (storage->ops->ftruncate(storage->ctx, storage->handle,
                                    storage->committed_offset) != GT_OK
            || storage->ops->fsync(storage->ctx, storage->handle) != GT_OK)) {
        /* A failed flush may have left torn bytes at the end of the old
         * segment. Once it stops being the newest, recovery never looks
         * at it again, so the tail must be cut now or not at all. */
        *events |= GT_STORAGE_EV_WRITE_ERROR;
        return GT_ERR_IO;
    }
    int new_handle = -1;
    if (gt_storage_create_segment(storage->ops, storage->ctx, today, now_us,
                                  &new_handle) != GT_OK) {
        *events |= GT_STORAGE_EV_SEGMENT_CREATE_ERROR;
        return GT_ERR_IO;
    }
    storage->ops->close(storage->ctx, storage->handle);
    storage->handle = new_handle;
    memcpy(storage->active_name, today, GT_FILE_NAME_MAX);
    storage->committed_offset = GT_SEG_HEADER_SZ;
    *events |= GT_STORAGE_EV_ROLLOVER;

    /* The window's first day: today minus (days - 1), never before the
     * epoch. */
    unsigned days = retention_days != 0 ? retention_days : GT_RETENTION_DAYS;
    uint64_t window_us = (uint64_t)(days - 1) * 86400u * 1000000u;
    char cutoff[GT_FILE_NAME_MAX];
    gt_segment_name(now_us > window_us ? now_us - window_us : 0, cutoff);
    delete_old_segments(storage, cutoff, days, events);
    return GT_OK;
}

/* handle, active_name and committed_offset belong to the flushing thread
 * (see flush), so the switch to the new segment needs no lock: append
 * never reads them. */
gt_status gt_storage_rollover(gt_storage *storage, uint64_t now_us, int clock_synced,
                              unsigned retention_days, unsigned *events_out)
{
    unsigned events = 0;
    gt_status status = GT_OK;
    char today[GT_FILE_NAME_MAX];

    if (storage == NULL || gt_segment_name(now_us, today) != GT_OK) {
        status = GT_ERR_INVAL;
    } else if (!clock_synced) {
        events |= GT_STORAGE_EV_RETENTION_SKIPPED;
    } else if (strcmp(today, storage->active_name) > 0) {
        status = switch_segment(storage, today, now_us, retention_days, &events);
    }
    if (events_out != NULL) {
        *events_out = events;
    }
    return status;
}

/* The marker is an empty file, so only its directory entry has to be
 * made durable: fsync_dir, no fsync of the file itself. */
gt_status gt_storage_shutdown(gt_storage *storage, unsigned *events_out)
{
    if (events_out != NULL) {
        *events_out = 0;
    }
    if (storage == NULL) {
        return GT_ERR_INVAL;
    }

    gt_status status = gt_storage_flush(storage, events_out);
    if (status != GT_OK) {
        return status;   /* no marker: this shutdown lost records */
    }

    int marker_handle = -1;
    if (storage->ops->open(storage->ctx, GT_STORAGE_MARKER_NAME, GT_FILE_CREATE,
                           &marker_handle) != GT_OK) {
        status = GT_ERR_IO;
    } else {
        storage->ops->close(storage->ctx, marker_handle);
        if (storage->ops->fsync_dir(storage->ctx) != GT_OK) {
            status = GT_ERR_IO;
        }
    }
    if (status != GT_OK && events_out != NULL) {
        *events_out |= GT_STORAGE_EV_WRITE_ERROR;
    }
    return status;
}
