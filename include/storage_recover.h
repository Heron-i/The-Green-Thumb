#ifndef STORAGE_RECOVER_H
#define STORAGE_RECOVER_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"
#include "storage_io.h"

/* Startup recovery scan (DESIGN.md "What happens when the power dies
 * mid-write", step 3), over a segment that is already in memory. Pure
 * logic: the caller reads the file, scans it, then truncates the file to
 * good_len (gt_storage_recover_ops, Mechanism D Step 6). Records are a
 * fixed GT_REC_SIZE, so the scan is offset arithmetic. */
typedef struct {
    int      header_ok;        /* 1 if the 16 B segment header is valid */
    size_t   good_len;         /* bytes to keep: header + every valid record */
    size_t   records_kept;     /* whole valid records before good_len */
    size_t   records_removed;  /* 24 B slots after good_len; a partial tail counts as 1 */
    int      has_records;      /* 1 if records_kept > 0, so last_seq means something */
    uint32_t last_seq;         /* seq of the last kept record; 0 if none */
    uint64_t last_timestamp_us;  /* timestamp of the last kept record; 0 if none */
} gt_scan_result;

/* Scan `len` bytes of one segment file. The scan stops at the first record
 * that fails gt_record_decode (bad magic, version, type, sensor_id or CRC),
 * whose seq is not greater than the one before it, or that is a partial
 * (< 24 B) tail; that record and everything after it are counted as removed.
 * seq must increase but may skip values, which GAP records explain.
 *
 * A bad header, or fewer than 16 bytes, gives header_ok = 0 with every
 * other field 0: the caller renames that file to .corrupt instead of
 * truncating it.
 *
 * GT_OK whenever the scan ran, even if it found damage (the damage is in
 * *out). GT_ERR_INVAL for NULL args, with *out untouched. */
gt_status gt_recover_scan(const uint8_t *bytes, size_t len, gt_scan_result *out);

/* What recovery did to one segment; the caller logs it as recovery_event
 * or segment_corrupt. */
typedef enum {
    GT_RECOVER_CLEAN,       /* nothing to cut; the file was not changed */
    GT_RECOVER_TRUNCATED,   /* a damaged tail was cut off and the file fsynced */
    GT_RECOVER_RENAMED      /* bad or short header: renamed to <name>.corrupt */
} gt_recover_outcome;

typedef struct {
    gt_recover_outcome outcome;
    uint64_t bytes_removed;    /* TRUNCATED: bytes cut; RENAMED: the whole file */
    size_t   records_removed;  /* TRUNCATED: as gt_scan_result; otherwise 0 */
    size_t   records_kept;     /* valid records left in the file; 0 if RENAMED */
    int      has_records;      /* 1 if records_kept > 0 */
    uint32_t last_seq;         /* seq of the last kept record; 0 if none */
    uint64_t last_timestamp_us;  /* timestamp of the last kept record; 0 if none */
} gt_recover_report;

/* Recover the segment `segment_name` in the directory behind ops/ctx
 * (DESIGN.md "What happens when the power dies mid-write", steps 2-4):
 * read the whole file into memory, scan it with gt_recover_scan, then
 *   - nothing to cut: change nothing (no write, no fsync);
 *   - a damaged tail: ftruncate to good_len, then fsync;
 *   - a bad or short header: rename to "<segment_name>.corrupt" (replacing
 *     an older one with that name), then fsync_dir so the rename survives
 *     a power cut. The caller starts a fresh segment.
 * Every handle is closed and all memory freed before it returns.
 *   GT_OK         *report_out says what happened
 *   GT_ERR_IO     the file is missing, or any file op failed; *report_out
 *                 is untouched and the caller retries or reports it
 *   GT_ERR_NOMEM  the file could not be read into memory
 *   GT_ERR_INVAL  NULL args, or segment_name + ".corrupt" doesn't fit
 *                 GT_FILE_NAME_MAX */
gt_status gt_storage_recover_ops(const gt_file_ops *ops, void *ctx,
                                 const char *segment_name, gt_recover_report *report_out);

#endif
