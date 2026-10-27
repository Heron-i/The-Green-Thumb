#ifndef STORAGE_PENDING_H
#define STORAGE_PENDING_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"
#include "log_record.h"
#include "storage.h"

/* Pending buffer: every record storage has accepted but not yet committed
 * (DESIGN.md "How writes are batched and when data is flushed").
 *
 * One buffer does both jobs DESIGN.md describes: it is the batch in normal
 * operation (a flush is due at GT_BATCH_MAX_RECORDS records or
 * GT_BATCH_MAX_AGE_S seconds), and the outage queue when flushes fail,
 * because records stay here until a flush succeeds. Records are kept as
 * the raw 24 bytes storage received, already checked by gt_record_decode,
 * so a flush writes them to disk unchanged. The buffer does not check
 * them itself.
 *
 * Each record keeps the time it was added (8 B more per record, about
 * 350 KB at full capacity), so the age rule is exact even for a record
 * that arrives while a flush is running. That keeps DESIGN.md's commit
 * latency target (max < 62 s) honest.
 *
 * Pure logic, no lock: gt_storage holds the mutex around every call.
 * Times are a monotonic clock in ms, passed in so tests need no clock. */
#define GT_PENDING_MAX_RECORDS (1024u * 1024u / GT_REC_SIZE)   /* 1 MB = 43,690 records */

typedef struct gt_pending gt_pending;

/* A buffer that holds up to `capacity_records` records: GT_PENDING_MAX_RECORDS
 * in storage, a small number in tests. The space is allocated once here and
 * never grows. NULL for a capacity of 0 or no memory. */
gt_pending *gt_pending_create(size_t capacity_records);

/* Append one record, copying its 24 bytes, and remember now_ms as the
 * time it was added.
 *   GT_OK        added
 *   GT_ERR_FULL  the buffer is full; nothing changes (the caller refuses
 *                and counts the reading)
 *   GT_ERR_INVAL NULL args */
gt_status   gt_pending_add(gt_pending *pending, const uint8_t record[GT_REC_SIZE],
                           uint64_t now_ms);

/* Records waiting right now. 0 for NULL. */
size_t      gt_pending_count(const gt_pending *pending);

/* Change when a flush is due (PF-11): at batch_records records waiting
 * (N), or once the oldest has waited max_age_ms (T). A new buffer starts
 * at GT_BATCH_MAX_RECORDS and GT_BATCH_MAX_AGE_S; this lets storage_main
 * run the (N, T) experiment without a rebuild. Records already waiting
 * keep their add times, so a new T applies to them at once.
 *   GT_OK        changed
 *   GT_ERR_INVAL NULL buffer, a 0 batch or age, or a batch larger than
 *                the capacity (it could never fill); nothing changes */
gt_status   gt_pending_set_limits(gt_pending *pending, size_t batch_records,
                                  uint64_t max_age_ms);

/* 1 if a flush is due: at least the batch size (N) of records are waiting,
 * or the oldest waiting record was added the age limit (T) ago or more
 * (GT_BATCH_MAX_RECORDS and GT_BATCH_MAX_AGE_S unless changed by
 * gt_pending_set_limits). An empty buffer is never due. 0 for NULL. */
int         gt_pending_due(const gt_pending *pending, uint64_t now_ms);

/* The waiting records, oldest first, as one block of
 * *count_out * GT_REC_SIZE bytes. Nothing is removed: the caller copies
 * the block out, writes it, and calls gt_pending_commit only once the
 * write and fsync succeed. NULL (and *count_out = 0) for a NULL buffer;
 * NULL for a NULL count_out. */
const uint8_t *gt_pending_peek(const gt_pending *pending, size_t *count_out);

/* Drop the oldest `record_count` records, now that they are on disk.
 * Records added after the peek stay, in order, each keeping the time it
 * was added.
 *   GT_OK        dropped
 *   GT_ERR_INVAL NULL buffer, or record_count > the records waiting;
 *                nothing is dropped */
gt_status   gt_pending_commit(gt_pending *pending, size_t record_count);

/* Free the buffer. NULL is a no-op. */
void        gt_pending_destroy(gt_pending *pending);

#endif
