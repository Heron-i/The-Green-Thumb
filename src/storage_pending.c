/* Pending buffer: accepted records waiting to be committed (see
 * storage_pending.h).
 *
 * Purpose: hold every record between storage accepting it and fsync
 * returning for it, so a failed flush loses nothing, and say when a flush
 * is due (N = 64 records or T = 60 s, DESIGN.md "How writes are batched").
 *
 * Layout: two arrays allocated once at create time, filled from index 0.
 * Record i's 24 bytes are at records[i * GT_REC_SIZE] and the time it was
 * added is added_ms[i], so the oldest record is always index 0 and peek
 * can hand out one contiguous block. Commit slides the leftovers down to
 * index 0 with memmove. That copies at most the records added during one
 * flush, normally zero or a handful.
 *
 * Pure logic: no files, no lock, no clock. */
#include "storage_pending.h"

#include <stdlib.h>
#include <string.h>

struct gt_pending {
    size_t    capacity;   /* records the arrays can hold */
    size_t    count;      /* records waiting, at indexes 0 .. count - 1 */
    uint8_t  *records;    /* capacity * GT_REC_SIZE bytes */
    uint64_t *added_ms;   /* capacity add times, same order as records */
};

/* malloc returns NULL when it can't allocate, and free(NULL) is allowed,
 * so both arrays can be freed together on any failure. SIZE_MAX is the
 * largest size_t; the check stops capacity * GT_REC_SIZE from wrapping
 * around to a small number. */
gt_pending *gt_pending_create(size_t capacity_records)
{
    if (capacity_records == 0 || capacity_records > SIZE_MAX / GT_REC_SIZE) {
        return NULL;
    }
    gt_pending *pending = malloc(sizeof *pending);
    if (pending == NULL) {
        return NULL;
    }
    pending->capacity = capacity_records;
    pending->count = 0;
    pending->records = malloc(capacity_records * GT_REC_SIZE);
    pending->added_ms = malloc(capacity_records * sizeof *pending->added_ms);
    if (pending->records == NULL || pending->added_ms == NULL) {
        gt_pending_destroy(pending);
        return NULL;
    }
    return pending;
}

gt_status gt_pending_add(gt_pending *pending, const uint8_t record[GT_REC_SIZE],
                         uint64_t now_ms)
{
    if (pending == NULL || record == NULL) {
        return GT_ERR_INVAL;
    }
    if (pending->count == pending->capacity) {
        return GT_ERR_FULL;
    }
    memcpy(&pending->records[pending->count * GT_REC_SIZE], record, GT_REC_SIZE);
    pending->added_ms[pending->count] = now_ms;
    pending->count++;
    return GT_OK;
}

size_t gt_pending_count(const gt_pending *pending)
{
    return pending == NULL ? 0 : pending->count;
}

/* The age check is written as now_ms >= oldest + limit (not
 * now_ms - oldest >= limit), so a now_ms earlier than the oldest add
 * can't wrap around to a huge age. */
int gt_pending_due(const gt_pending *pending, uint64_t now_ms)
{
    if (pending == NULL || pending->count == 0) {
        return 0;
    }
    if (pending->count >= GT_BATCH_MAX_RECORDS) {
        return 1;
    }
    uint64_t age_limit_ms = (uint64_t)GT_BATCH_MAX_AGE_S * 1000u;
    uint64_t oldest_ms = pending->added_ms[0];
    return oldest_ms <= UINT64_MAX - age_limit_ms && now_ms >= oldest_ms + age_limit_ms;
}

const uint8_t *gt_pending_peek(const gt_pending *pending, size_t *count_out)
{
    if (count_out == NULL) {
        return NULL;
    }
    if (pending == NULL) {
        *count_out = 0;
        return NULL;
    }
    *count_out = pending->count;
    return pending->records;
}

/* memmove (not memcpy) because the source and destination ranges can
 * overlap when more records are left than were committed. */
gt_status gt_pending_commit(gt_pending *pending, size_t record_count)
{
    if (pending == NULL || record_count > pending->count) {
        return GT_ERR_INVAL;
    }
    size_t left = pending->count - record_count;
    memmove(pending->records, &pending->records[record_count * GT_REC_SIZE],
            left * GT_REC_SIZE);
    memmove(pending->added_ms, &pending->added_ms[record_count],
            left * sizeof *pending->added_ms);
    pending->count = left;
    return GT_OK;
}

void gt_pending_destroy(gt_pending *pending)
{
    if (pending == NULL) {
        return;
    }
    free(pending->records);
    free(pending->added_ms);
    free(pending);
}
