#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"
#include "log_record.h"
#include "storage_io.h"
#include "storage_recover.h"

#define GT_BATCH_MAX_RECORDS 64   /* N */
#define GT_BATCH_MAX_AGE_S   60   /* T */

/* Segment file names (DESIGN.md "How records are organized"): one file per
 * UTC day, "log-YYYYMMDD.bin". The fixed width means sorting names
 * alphabetically also sorts them by date. */

/* Write the segment name for the UTC day of now_us (us since the Unix
 * epoch) into name_out. GT_ERR_INVAL for a NULL name_out or a year past
 * 9999 (it would not fit 4 digits); name_out is untouched on failure. */
gt_status gt_segment_name(uint64_t now_us, char name_out[GT_FILE_NAME_MAX]);

/* 1 if `name` is exactly "log-" + 8 digits + ".bin", else 0 (NULL too).
 * Storage uses it to skip every other file in the directory: .corrupt
 * segments, the clean-shutdown marker and the lock file. Only the shape
 * is checked, not that the digits are a real date. */
int       gt_is_segment_name(const char *name);

typedef struct gt_storage gt_storage;

/* The clean-shutdown marker: an empty file gt_storage_shutdown creates
 * after its final flush, and open deletes. If it is missing at open, the
 * last run ended without a clean shutdown (power cut, kill -9, crash). */
#define GT_STORAGE_MARKER_NAME "clean_shutdown"

/* What gt_storage_open_ops found and did, for the caller to log
 * (unclean_shutdown, recovery_event, segment_corrupt) and to answer the
 * hub's seq_resume. */
typedef struct {
    int      unclean_shutdown;               /* 1: segments exist but the marker was missing */
    char     active_name[GT_FILE_NAME_MAX];  /* the segment new records go to */
    int      created;                        /* 1 if open made that segment */
    int      recovered;                      /* 1 if a segment existed and was recovered */
    char     recovered_name[GT_FILE_NAME_MAX];  /* the newest segment, if recovered */
    gt_recover_report recovery;              /* valid when recovered is 1 */
    int      has_last;                       /* 1 if any segment holds a record */
    uint32_t last_seq;                       /* seq of the newest record on disk */
} gt_storage_open_info;

/* Counters for tests, heartbeats and the seq_resume reply. A pair of
 * has_* / *_seq fields is needed because seq 0 is a valid value. */
typedef struct {
    size_t   pending;              /* records accepted, not yet committed */
    uint64_t committed_records;    /* records fsynced since open */
    int      has_last_accepted;
    uint32_t last_accepted_seq;    /* newest record accepted (maybe still pending) */
    int      has_last_committed;
    uint32_t last_committed_seq;   /* newest record on disk */
    int      degraded;             /* 1 from a failed flush until one succeeds */
    unsigned backlog_percent;      /* highest threshold reached now: 0, 50, 75 or 90 */
    uint64_t refused;              /* readings refused because the buffer was full */
} gt_storage_stats;

/* Open storage over the directory behind ops/ctx, running recovery before
 * anything else (DESIGN.md "recovery runs before storage.sock accepts any
 * data"):
 *   0. Check for the clean-shutdown marker (DESIGN.md recovery step 1).
 *      Missing while segments exist means unclean_shutdown; a brand-new
 *      directory has nothing to recover, so that is not reported. A marker
 *      that is there is deleted and fsync_dir'd at once, so it can't
 *      survive into a run that might itself end in a crash. The marker
 *      only decides what is logged: recovery always runs.
 *   1. List the directory and keep only segment names, oldest first.
 *   2. Recover the newest segment with gt_storage_recover_ops. Only the
 *      file being appended to can have a damaged tail.
 *   3. Find the newest seq on disk: if the newest segment has no records
 *      (header-only, or renamed .corrupt), look back through the older
 *      ones until one does.
 *   4. Append to the newest remaining segment if its name is today's or
 *      later; otherwise create today's. "Or later" keeps the log in date
 *      order when the clock is behind (a Pi that booted without network
 *      time thinks it is 1970).
 *   5. A new segment gets its header written, then fsync, then fsync_dir,
 *      so it can't vanish in a power cut. A half-made file left by a
 *      failure here is renamed .corrupt by the next open's recovery.
 * now_us is the wall clock in us since the epoch; it picks today's segment
 * and stamps a new header. pending_capacity is how many records may wait
 * to be committed: 0 means GT_PENDING_MAX_RECORDS (1 MB); tests pass a
 * small number. info_out may be NULL.
 *
 * Ownership, as for gt_ipc_client_open_ops: once ops is non-NULL, storage
 * owns ctx and calls ops->destroy(ctx) exactly once, from gt_storage_close,
 * or before returning if open fails for any reason. With NULL ops the
 * caller still owns ctx.
 *
 * `storage_out` is a pointer-to-pointer (`**`), as in ipc_sock.h: on GT_OK,
 * *storage_out is set to the new storage; on failure it is untouched.
 *   GT_OK, GT_ERR_IO (any file op failed), GT_ERR_NOMEM, GT_ERR_INVAL
 *   (NULL ops or storage_out, or a now_us past the year 9999) */
gt_status   gt_storage_open_ops(const gt_file_ops *ops, void *ctx, uint64_t now_us,
                                size_t pending_capacity, gt_storage **storage_out,
                                gt_storage_open_info *info_out);

/* Copy the counters into *stats_out. GT_ERR_INVAL for NULL args. */
gt_status   gt_storage_read_stats(const gt_storage *storage, gt_storage_stats *stats_out);

/* Opens the segment dir and runs recovery before accepting data. */
gt_storage *gt_storage_open(const char *dir);
/* Scan one segment; truncate at the first bad record. */
gt_status   gt_storage_recover(const char *segment_path, size_t *removed_records);
/* Event flags from gt_storage_append, one bit each like GT_IPC_EV_*; the
 * caller logs the matching event name. */
#define GT_STORAGE_EV_SEQ_REJECTED 0x1u   /* a duplicate or old seq: log seq_rejected */
#define GT_STORAGE_EV_RECOVERY_GAP 0x2u   /* seqs were missing: log recovery_gap */
/* Event flags from gt_storage_flush (DESIGN.md failure-mode table,
 * Storage "write() or fsync() fails" row). */
#define GT_STORAGE_EV_WRITE_ERROR  0x4u   /* write or truncate failed: storage_write_error */
#define GT_STORAGE_EV_FSYNC_ERROR  0x8u   /* fsync failed: fsync_error */
#define GT_STORAGE_EV_DEGRADED     0x10u  /* just became degraded: storage_degraded */
#define GT_STORAGE_EV_RECOVERED    0x20u  /* first good flush after degraded: storage_recovered */
/* Event flags from gt_storage_append for a filling buffer (DESIGN.md
 * failure-mode table, Storage "backlog fills during an outage" row). */
#define GT_STORAGE_EV_BACKLOG      0x40u  /* just crossed 50/75/90% going up: storage_backlog */
#define GT_STORAGE_EV_QUEUE_FULL   0x80u  /* first refusal while full: storage_degraded */

/* Accept one 24 B record as it arrived on storage.sock. Under the lock it
 * is checked with gt_record_decode, then its seq with gt_seq_check against
 * the last accepted seq (the newest on disk, right after open), then it
 * is copied into the pending buffer unchanged. now_ms is a monotonic clock
 * in ms; it starts the record's 60 s flush clock.
 *   GT_OK          accepted. With GT_STORAGE_EV_RECOVERY_GAP, seqs were
 *                  missing, so one GAP record (gt_seq_gap_record) was
 *                  stored just before it: both or neither.
 *   GT_ERR_CORRUPT refused. With GT_STORAGE_EV_SEQ_REJECTED it was a
 *                  duplicate or old seq (keep the connection); with no
 *                  event the bytes were bad (log ipc_bad_message and close
 *                  the connection).
 *   GT_ERR_FULL    refused: no room for it (or for it and its GAP), and
 *                  counted in gt_storage_stats.refused. The first refusal
 *                  of a "full episode" adds GT_STORAGE_EV_QUEUE_FULL; the
 *                  episode ends when a record is accepted again.
 *   GT_ERR_INVAL   NULL storage or record.
 * Nothing is stored on any refusal, and the last accepted seq is
 * unchanged. That is what makes refused readings explicit: the next
 * accepted record skips their seqs, so a GAP starting at the first
 * refused seq goes in before it (DESIGN.md "Gaps created by storage use
 * the seq of the first refused reading").
 *
 * GT_STORAGE_EV_BACKLOG is added when an accepted record takes the buffer
 * across 50%, 75% or 90% going up (pending * 100 >= capacity * threshold).
 * A flush that drains it below a threshold re-arms it, silently.
 * *events_out is set to 0 first; pass NULL to ignore it. */
gt_status   gt_storage_append(gt_storage *storage, const uint8_t record[GT_REC_SIZE],
                              uint64_t now_ms, unsigned *events_out);
/* 1 if a flush is due (GT_BATCH_MAX_RECORDS records pending, or the oldest
 * pending record is GT_BATCH_MAX_AGE_S old; see gt_pending_due), else 0.
 * now_ms is the same monotonic clock passed to append. 0 for NULL. */
int         gt_storage_flush_due(gt_storage *storage, uint64_t now_ms);

/* Commit every pending record (DESIGN.md "Commit sequence"):
 *   1. lock, copy the pending records into the flush buffer, unlock;
 *   2. write them at the end of the active segment, then fsync;
 *   3. lock, drop them from the pending buffer and advance the committed
 *      offset and seq, unlock.
 * A record is committed once this returns GT_OK. Records appended while
 * the write is running stay pending for the next flush. Only one thread
 * may flush at a time (the flush thread).
 *
 * Failures never lose or duplicate a record. A failed write or fsync
 * keeps every record pending and marks storage degraded. The next flush
 * first truncates the segment back to the committed offset, cutting off
 * any partly written (torn) bytes, then writes the records again and
 * fsyncs, so the file never holds a partial record or the same record
 * twice.
 *   GT_OK        committed (also when nothing was pending: then nothing
 *                is written and nothing fsynced). GT_STORAGE_EV_RECOVERED
 *                if storage was degraded.
 *   GT_ERR_IO    the truncate, write or fsync failed; every record stays
 *                pending. GT_STORAGE_EV_WRITE_ERROR (truncate or write) or
 *                GT_STORAGE_EV_FSYNC_ERROR, plus GT_STORAGE_EV_DEGRADED
 *                the first time only.
 *   GT_ERR_INVAL NULL storage
 * *events_out is set to 0 first; pass NULL to ignore it. */
gt_status   gt_storage_flush(gt_storage *storage, unsigned *events_out);
/* Clean shutdown (DESIGN.md "On SIGTERM, the flush thread commits whatever
 * is buffered, then writes a clean-shutdown marker"): flush every pending
 * record, then create GT_STORAGE_MARKER_NAME and fsync_dir so the marker
 * is durable. The marker is written only if the flush succeeded, so it
 * never claims a shutdown lost nothing when it did. Call it once, right
 * before gt_storage_close, and append nothing after it.
 *   GT_OK        everything committed and the marker is on disk
 *   GT_ERR_IO    the flush failed (its events, no marker), or the marker
 *                could not be made (GT_STORAGE_EV_WRITE_ERROR; the records
 *                are committed, only the marker is missing, so the next
 *                open reports an unclean shutdown that wasn't one)
 *   GT_ERR_INVAL NULL storage
 * *events_out is set to 0 first; pass NULL to ignore it. */
gt_status   gt_storage_shutdown(gt_storage *storage, unsigned *events_out);

/* Close the segment, free everything and call ops->destroy(ctx) once.
 * It writes nothing: pending records are dropped. Call gt_storage_shutdown
 * first for a clean shutdown. NULL is a no-op. */
void        gt_storage_close(gt_storage *storage);

#endif
