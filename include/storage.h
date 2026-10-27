#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"
#include "log_record.h"
#include "gt_log.h"
#include "storage_io.h"
#include "storage_proto.h"
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

/* List the directory behind ops/ctx and keep only segment names, sorted
 * oldest first (alphabetical order is date order). *marker_out is 1 if
 * the clean-shutdown marker is there too. Storage, retention and the log
 * verifier all read segments in this one order. GT_OK, or GT_ERR_IO if
 * the listing fails. */
gt_status gt_storage_list_segments(const gt_file_ops *ops, void *ctx,
                                   char segments[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX],
                                   size_t *count_out, int *marker_out);

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
    uint64_t last_timestamp_us;              /* that record's timestamp: a GAP found after
                                              * this restart is stamped with it (PF-07) */
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
    uint64_t segments_deleted;     /* old segments removed by retention since open */
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

/* gt_storage_open_ops over the real directory `dir`, through
 * gt_file_ops_linux, with the default 1 MB pending buffer. What storage_main
 * calls; tests use gt_storage_open_ops with tests/fake_file.h instead.
 * Returns what gt_file_ops_linux or gt_storage_open_ops returns. */
gt_status   gt_storage_open(const char *dir, uint64_t now_us, gt_storage **storage_out,
                            gt_storage_open_info *info_out);
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
/* Event flags from gt_storage_rollover (DESIGN.md "With what retention",
 * and the failure-mode table's segment_create_error, retention_skipped
 * and retention_error rows). */
#define GT_STORAGE_EV_ROLLOVER             0x100u  /* a new day's segment was created */
#define GT_STORAGE_EV_SEGMENT_CREATE_ERROR 0x200u  /* it could not be: segment_create_error */
#define GT_STORAGE_EV_RETENTION_SKIPPED    0x400u  /* clock unsynced: retention_skipped */
#define GT_STORAGE_EV_RETENTION_ERROR      0x800u  /* an old segment could not be deleted */

#define GT_RETENTION_DAYS 30   /* segments kept: today and the 29 days before it */
/* Event flag from gt_storage_rx (DESIGN.md failure-mode table, Storage
 * "Malformed message on storage.sock" row). */
#define GT_STORAGE_EV_BAD_MESSAGE          0x1000u /* wrong length or bad bytes: ipc_bad_message */

/* How one GT_STORAGE_EV_* flag is logged (DESIGN.md failure-mode table,
 * Storage rows), so the mapping lives in one tested place instead of in
 * storage_main. `reason` tells apart two events that share a name: a
 * failed write (DEGRADED) and a full buffer (QUEUE_FULL) are both
 * storage_degraded, logged with reason=write_failed or reason=queue_full;
 * it is NULL for every other event. */
typedef struct {
    const char  *name;     /* the log event name, never containing a space */
    gt_log_level level;
    const char  *reason;   /* value for a reason=... field, or NULL */
} gt_storage_event;

/* Describe exactly one flag. GT_ERR_INVAL for 0, an unknown bit, more than
 * one bit set, or NULL out; *out is untouched on failure. The caller logs
 * an events word by describing each set bit in turn. */
gt_status   gt_storage_event_describe(unsigned flag, gt_storage_event *out);

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
 * the seq of the first refused reading"). Every storage-made GAP is
 * stamped with the last accepted record's time, the moment the gap could
 * first have started; after open that is the newest record on disk
 * (PF-07).
 *
 * GT_STORAGE_EV_BACKLOG is added when an accepted record takes the buffer
 * across 50%, 75% or 90% going up (pending * 100 >= capacity * threshold).
 * A flush that drains it below a threshold re-arms it, silently.
 * *events_out is set to 0 first; pass NULL to ignore it. */
gt_status   gt_storage_append(gt_storage *storage, const uint8_t record[GT_REC_SIZE],
                              uint64_t now_ms, unsigned *events_out);
/* What to do with the hub's connection after one received frame. */
typedef enum {
    GT_RX_KEEP,    /* go on reading from it */
    GT_RX_CLOSE    /* close it; the hub reconnects and gets a fresh seq_resume */
} gt_rx_action;

/* Handle one frame from storage.sock. `len` is recv()'s return value, so
 * it is signed (`long`), as in gt_ipc_frame_decode: -1 means recv()
 * failed and 0 means the hub closed the connection. The caller recv()s
 * into a GT_REC_SIZE + 1 buffer, because SOCK_SEQPACKET silently cuts off
 * a message longer than the buffer; the extra byte makes an oversized
 * frame show up as 25.
 *   len -1 or 0           CLOSE, no event (a disconnect, not a bad message)
 *   any other len but 24  CLOSE + GT_STORAGE_EV_BAD_MESSAGE
 *   24                    gt_storage_append: bad bytes are CLOSE +
 *                         BAD_MESSAGE; everything else is KEEP, with
 *                         append's events (a duplicate seq or a full
 *                         buffer is refused, but the hub is fine, so the
 *                         connection stays)
 * NULL storage or buf is CLOSE with no event. *events_out is set to 0
 * first; pass NULL to ignore it. */
gt_rx_action gt_storage_rx(gt_storage *storage, const uint8_t *buf, long len,
                           uint64_t now_ms, unsigned *events_out);

/* Fill *resume_out with the seq_resume reply for a newly connected hub:
 * the last accepted seq (see storage_proto.h for why not the last
 * committed). GT_ERR_INVAL for NULL args. */
gt_status   gt_storage_resume(const gt_storage *storage, gt_resume *resume_out);

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
/* Daily rollover and retention. The flush thread calls this just before
 * each flush, and only that thread may call it (it changes the active
 * segment, which only the flushing thread touches).
 *   1. If clock_synced is 0, do nothing and report RETENTION_SKIPPED
 *      (DESIGN.md: skip rollover and retention while the clock is
 *      unsynced; keep appending to the current segment). Storage can't
 *      tell for itself: storage_main passes what adjtimex says.
 *   2. If the UTC day of now_us is later than the active segment's, first
 *      flush every pending record into the old segment (gt_storage_flush,
 *      its events included: WRITE_ERROR / FSYNC_ERROR, DEGRADED,
 *      RECOVERED), so a day's records stay in that day's file (PF-06).
 *      Records that arrive between midnight and this call still count as
 *      the old day's: at most one flusher period (1 s). Then create today's
 *      segment (header, fsync, fsync_dir, as in open) and make it active:
 *      ROLLOVER. If the pre-flush failed, the records stay pending and go
 *      into the new segment at the next flush. If the day is the same or
 *      earlier (the clock went back), keep the active segment. If storage
 *      is degraded (including after a failed pre-flush), the old segment
 *      may end in torn bytes from the failed flush, and recovery only
 *      ever repairs the newest segment, so the old one is first cut back
 *      to its committed offset and fsynced. If that cut fails, there is
 *      no rollover this time (WRITE_ERROR, GT_ERR_IO); the next call
 *      tries again.
 *   3. Only right after a rollover, delete old segments. A segment is
 *      deleted only if BOTH its date is before the window (today and the
 *      retention_days - 1 days before it) AND it is not one of the newest
 *      retention_days segments. The second rule means a clock that jumps
 *      years ahead can't wipe out the history: retention only ever trims
 *      beyond retention_days files. The active segment, .corrupt files
 *      and every other non-segment file are never deleted.
 * retention_days 0 means GT_RETENTION_DAYS; tests pass a small number.
 *
 * Known limitation: a Pi that boots with no network time and an empty
 * log starts writing log-19700101.bin. That file is outside the window
 * at once, but the newest-N rule keeps it until N newer segments exist.
 *   GT_OK        done (also for "nothing to do" and RETENTION_SKIPPED). A
 *                failed delete adds RETENTION_ERROR and the rest go on:
 *                history is never at risk.
 *   GT_ERR_IO    today's segment could not be created
 *                (SEGMENT_CREATE_ERROR), or a degraded old segment could
 *                not be cut back (WRITE_ERROR); the old segment stays
 *                active, and the next call tries again
 *   GT_ERR_INVAL NULL storage, or a now_us past the year 9999
 * *events_out is set to 0 first; pass NULL to ignore it. */
gt_status   gt_storage_rollover(gt_storage *storage, uint64_t now_us, int clock_synced,
                                unsigned retention_days, unsigned *events_out);

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
