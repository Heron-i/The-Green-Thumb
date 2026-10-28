/* Opening and closing storage (see storage.h).
 *
 * Purpose: everything that happens before the first append and after the
 * last one: making the storage object (memory first, so running out of it
 * never leaves a half-made segment), reading and deleting the
 * clean-shutdown marker, recovering the newest segment and walking back
 * for the last seq on disk, picking or durably creating the active
 * segment, and finally freeing it all on close.
 *
 * All file access goes through the gt_file_ops seam (storage_io.h), so
 * tests run on tests/fake_file.h. */
#include "storage_internal.h"

#include <stdlib.h>
#include <string.h>

#include "storage_recover.h"

/* Close the active segment, free everything and destroy ctx: the one
 * clean-up path for both close and a failed open. */
static void free_storage(gt_storage *storage)
{
    if (storage->handle >= 0) {
        storage->ops->close(storage->ctx, storage->handle);
    }
    if (storage->cond_ready) {
        pthread_cond_destroy(&storage->due_cond);
    }
    if (storage->lock_ready) {
        pthread_mutex_destroy(&storage->lock);
    }
    gt_pending_destroy(storage->pending);
    free(storage->flush_buf);
    storage->ops->destroy(storage->ctx);
    free(storage);
}

/* Make the condition variable on WAIT_CLOCK. A pthread_condattr_t holds
 * the options for pthread_cond_init and is only needed until then.
 * Returns 1 on success. */
static int init_due_cond(gt_storage *storage)
{
    pthread_condattr_t options;
    if (pthread_condattr_init(&options) != 0) {
        return 0;
    }
    int ok = 1;
#ifdef __linux__
    ok = pthread_condattr_setclock(&options, WAIT_CLOCK) == 0;
#endif
    ok = ok && pthread_cond_init(&storage->due_cond, &options) == 0;
    pthread_condattr_destroy(&options);
    return ok;
}

/* The memory half of open, done before any file is touched, so running
 * out of memory never leaves a half-made segment. */
static gt_status alloc_storage(const gt_file_ops *ops, void *ctx, size_t pending_capacity,
                               gt_storage **storage_out)
{
    gt_storage *storage = calloc(1, sizeof *storage);   /* calloc zeroes the struct */
    if (storage == NULL) {
        return GT_ERR_NOMEM;
    }
    storage->ops = ops;
    storage->ctx = ctx;
    storage->handle = -1;
    storage->capacity = pending_capacity != 0 ? pending_capacity : GT_PENDING_MAX_RECORDS;
    storage->pending = gt_pending_create(storage->capacity);   /* checks capacity * 24 fits */
    if (storage->pending != NULL) {
        storage->flush_buf = malloc(storage->capacity * GT_REC_SIZE);
    }
    if (storage->flush_buf != NULL && pthread_mutex_init(&storage->lock, NULL) == 0) {
        storage->lock_ready = 1;
        storage->cond_ready = init_due_cond(storage);
    }
    if (!storage->lock_ready || !storage->cond_ready) {
        free_storage(storage);   /* also destroys ctx: storage owns it now */
        return GT_ERR_NOMEM;
    }
    *storage_out = storage;
    return GT_OK;
}

/* Step 0 of open: note whether the last run ended cleanly (a log with
 * segments but no marker did not), then delete the marker durably at
 * once, so a crash from here on reads as unclean next time. */
static gt_status take_marker(const gt_file_ops *ops, void *ctx, int marker_present,
                             size_t segment_count, gt_storage_open_info *info)
{
    info->unclean_shutdown = !marker_present && segment_count > 0;
    if (!marker_present) {
        return GT_OK;
    }
    if (ops->unlink(ctx, GT_STORAGE_MARKER_NAME) != GT_OK || ops->fsync_dir(ctx) != GT_OK) {
        return GT_ERR_IO;
    }
    return GT_OK;
}

/* Put one segment's recovery report into info. The newest segment's
 * always goes in `recovery`. An older one is listed only if recovery
 * changed it (PF-14), and only counted once the list is full. */
static void note_recovery(gt_storage_open_info *info, int is_newest, const char *name,
                          const gt_recover_report *report)
{
    if (is_newest) {
        info->recovered = 1;
        memcpy(info->recovered_name, name, GT_FILE_NAME_MAX);
        info->recovery = *report;
        return;
    }
    if (report->outcome == GT_RECOVER_CLEAN) {
        return;
    }
    if (info->older_count >= GT_OPEN_OLDER_MAX) {
        info->older_unlisted++;
        return;
    }
    gt_older_recovery *older = &info->older[info->older_count++];
    memcpy(older->name, name, GT_FILE_NAME_MAX);
    older->report = *report;
}

/* Steps 2 and 3 of open: recover the newest segment, then walk back until
 * a segment with records gives the newest seq on disk. Older segments
 * were complete when the day rolled over, so recovering them normally
 * changes nothing; it is just how their last seq is read. On return,
 * *newest_out is the index of the newest segment that still exists, or
 * -1 if every segment visited was renamed .corrupt (or there are none). */
static gt_status find_newest(gt_storage *storage,
                             char segments[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX],
                             size_t count, gt_storage_open_info *info, long *newest_out)
{
    long newest = -1;
    /* invariant: segments[visit .. count - 1] have been recovered, and
     * none of them that still exists holds a record */
    for (size_t visit = count; visit > 0; visit--) {
        const char *name = segments[visit - 1];
        gt_recover_report report;
        gt_status status = gt_storage_recover_ops(storage->ops, storage->ctx, name, &report);
        if (status != GT_OK) {
            return status;
        }
        note_recovery(info, visit == count, name, &report);
        if (report.outcome == GT_RECOVER_RENAMED) {
            continue;
        }
        if (newest < 0) {
            newest = (long)(visit - 1);
        }
        if (report.has_records) {
            info->has_last = 1;
            info->last_seq = report.last_seq;
            info->last_timestamp_us = report.last_timestamp_us;
            break;
        }
    }
    *newest_out = newest;
    return GT_OK;
}

/* Declared in storage_internal.h: open and rollover both use it. On GT_OK
 * *handle_out is the open segment; on failure no handle is left open, so
 * a caller's current segment is never disturbed. If a file of that name
 * is already there (a half-made one from an earlier failure), it is
 * reused and its header rewritten. */
gt_status gt_storage_create_segment(const gt_file_ops *ops, void *ctx, const char *name,
                                    uint64_t now_us, int *handle_out)
{
    int handle = -1;
    if (ops->open(ctx, name, GT_FILE_CREATE, &handle) != GT_OK) {
        return GT_ERR_IO;
    }
    const gt_seg_header header = { .created_at_us = now_us };
    uint8_t header_bytes[GT_SEG_HEADER_SZ];
    if (gt_seg_header_encode(&header, header_bytes) != GT_OK
        || ops->pwrite(ctx, handle, 0, header_bytes, sizeof header_bytes) != GT_OK
        || ops->fsync(ctx, handle) != GT_OK
        || ops->fsync_dir(ctx) != GT_OK) {
        ops->close(ctx, handle);
        return GT_ERR_IO;
    }
    *handle_out = handle;
    return GT_OK;
}

/* Open the existing segment `name`; *size_out is its length, where the
 * next append goes. On failure no handle is left open. */
static gt_status open_existing(const gt_file_ops *ops, void *ctx, const char *name,
                               int *handle_out, uint64_t *size_out)
{
    int handle = -1;
    if (ops->open(ctx, name, 0, &handle) != GT_OK) {
        return GT_ERR_IO;
    }
    if (ops->size(ctx, handle, size_out) != GT_OK) {
        ops->close(ctx, handle);
        return GT_ERR_IO;
    }
    *handle_out = handle;
    return GT_OK;
}

/* Steps 4 and 5 of open: make `name` the active segment. An existing one
 * is opened and appended to at its end; a new one is made with
 * gt_storage_create_segment and appended to after its header. */
static gt_status open_active(gt_storage *storage, const char *name, int create, uint64_t now_us)
{
    int handle = -1;
    gt_status status = create
        ? gt_storage_create_segment(storage->ops, storage->ctx, name, now_us, &handle)
        : open_existing(storage->ops, storage->ctx, name, &handle, &storage->committed_offset);
    if (status != GT_OK) {
        return GT_ERR_IO;
    }
    if (create) {
        storage->committed_offset = GT_SEG_HEADER_SZ;
    }
    storage->handle = handle;
    memcpy(storage->active_name, name, GT_FILE_NAME_MAX);
    return GT_OK;
}

gt_status gt_storage_open_ops(const gt_storage_open_args *args, gt_storage **storage_out,
                              gt_storage_open_info *info_out)
{
    if (args == NULL || args->ops == NULL) {
        return GT_ERR_INVAL;   /* no destroy to call: the caller keeps ctx */
    }
    const gt_file_ops *ops = args->ops;
    void *ctx = args->ctx;
    uint64_t now_us = args->now_us;
    char today[GT_FILE_NAME_MAX];
    if (storage_out == NULL || gt_segment_name(now_us, today) != GT_OK) {
        ops->destroy(ctx);
        return GT_ERR_INVAL;
    }

    gt_storage *storage = NULL;
    gt_status status = alloc_storage(ops, ctx, args->pending_capacity, &storage);
    if (status != GT_OK) {
        return status;   /* alloc_storage already destroyed ctx */
    }

    gt_storage_open_info info;
    memset(&info, 0, sizeof info);
    char segments[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX];
    size_t segment_count = 0;
    int marker_present = 0;
    long newest = -1;

    status = gt_storage_list_segments(ops, ctx, segments, &segment_count, &marker_present);
    if (status == GT_OK) {
        status = take_marker(ops, ctx, marker_present, segment_count, &info);
    }
    if (status == GT_OK) {
        status = find_newest(storage, segments, segment_count, &info, &newest);
    }
    if (status == GT_OK) {
        /* Reuse the newest segment if it is today's or later (the clock may
         * be behind); otherwise start today's. */
        int reuse = newest >= 0 && strcmp(segments[newest], today) >= 0;
        info.created = !reuse;
        status = open_active(storage, reuse ? segments[newest] : today, !reuse, now_us);
    }
    if (status != GT_OK) {
        free_storage(storage);
        return status;
    }

    memcpy(info.active_name, storage->active_name, GT_FILE_NAME_MAX);
    storage->has_last_accepted = storage->has_last_committed = info.has_last;
    storage->last_accepted_seq = storage->last_committed_seq = info.last_seq;
    storage->last_accepted_us = info.last_timestamp_us;
    if (info_out != NULL) {
        *info_out = info;
    }
    *storage_out = storage;
    return GT_OK;
}

/* The real directory: the Linux file ops, then the same open as tests use.
 * If storage_out is NULL, gt_storage_open_ops still destroys the ctx. */
gt_status gt_storage_open(const char *dir, uint64_t now_us, gt_storage **storage_out,
                          gt_storage_open_info *info_out)
{
    const gt_file_ops *ops = NULL;
    void *ctx = NULL;
    gt_status status = gt_file_ops_linux(dir, &ops, &ctx);
    if (status != GT_OK) {
        return status;
    }
    gt_storage_open_args args = { .ops = ops, .ctx = ctx, .now_us = now_us, .pending_capacity = 0 };
    return gt_storage_open_ops(&args, storage_out, info_out);
}

void gt_storage_close(gt_storage *storage)
{
    if (storage == NULL) {
        return;
    }
    free_storage(storage);
}
