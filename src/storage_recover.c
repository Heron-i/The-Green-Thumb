/* Startup recovery scan over one in-memory segment (see storage_recover.h).
 *
 * Purpose: find the longest valid prefix of a segment file, so recovery
 * can truncate the file there before storage.sock accepts any data
 * (DESIGN.md "What happens when the power dies mid-write"). Only the tail
 * of an append-only log can be damaged, so everything up to the first bad
 * record is kept and everything from it on is cut.
 *
 * gt_recover_scan is pure logic: no files. gt_storage_recover_ops reads
 * the file into memory through the file seam (storage_io.h), scans it,
 * and acts on the result. */
#include "storage_recover.h"

#include <stdlib.h>
#include <string.h>

#include "log_record.h"

#define CORRUPT_SUFFIX ".corrupt"

gt_status gt_recover_scan(const uint8_t *bytes, size_t len, gt_scan_result *out)
{
    if (bytes == NULL || out == NULL) {
        return GT_ERR_INVAL;
    }

    gt_scan_result result;
    memset(&result, 0, sizeof result);

    gt_seg_header header;
    if (len < GT_SEG_HEADER_SZ || gt_seg_header_decode(bytes, &header) != GT_OK) {
        *out = result;   /* header_ok 0, everything else 0 */
        return GT_OK;
    }
    result.header_ok = 1;

    size_t offset = GT_SEG_HEADER_SZ;
    /* invariant: bytes [0, offset) are a valid header plus whole valid
     * records with strictly increasing seq, and result describes them */
    while (offset + GT_REC_SIZE <= len) {
        gt_record record;
        if (gt_record_decode(&bytes[offset], &record) != GT_OK) {
            break;
        }
        if (result.has_records && record.seq <= result.last_seq) {
            break;
        }
        result.has_records = 1;
        result.last_seq = record.seq;
        result.records_kept++;
        offset += GT_REC_SIZE;
    }

    /* Round up, so a partial trailing record counts as one removed slot. */
    size_t cut_bytes = len - offset;
    result.good_len = offset;
    result.records_removed = (cut_bytes + GT_REC_SIZE - 1) / GT_REC_SIZE;

    *out = result;
    return GT_OK;
}

/* Read the whole open file into a new malloc'd buffer. *bytes_out is NULL
 * for an empty file (there is nothing to allocate). The caller frees the
 * buffer with free(), which accepts NULL. A size that changes when it is
 * converted to size_t (only possible where size_t is 32 bits) can't be
 * held in memory at all. The check is written that way, not as
 * file_size > SIZE_MAX, because on 64-bit that comparison is always false
 * and gcc's -Wtype-limits (part of -Wextra) would fail the build. */
static gt_status read_whole_file(const gt_file_ops *ops, void *ctx, int handle,
                                 uint8_t **bytes_out, size_t *len_out)
{
    uint64_t file_size = 0;
    if (ops->size(ctx, handle, &file_size) != GT_OK) {
        return GT_ERR_IO;
    }
    if ((uint64_t)(size_t)file_size != file_size) {
        return GT_ERR_NOMEM;
    }
    *bytes_out = NULL;
    *len_out = (size_t)file_size;
    if (file_size == 0) {
        return GT_OK;
    }

    uint8_t *bytes = malloc((size_t)file_size);
    if (bytes == NULL) {
        return GT_ERR_NOMEM;
    }
    if (ops->pread(ctx, handle, 0, bytes, (size_t)file_size) != GT_OK) {
        free(bytes);
        return GT_ERR_IO;
    }
    *bytes_out = bytes;
    return GT_OK;
}

/* Move a segment with a bad header out of the way, then make the rename
 * durable: without fsync_dir a power cut could bring the bad file back
 * under its old name. */
static gt_status rename_corrupt(const gt_file_ops *ops, void *ctx, const char *segment_name)
{
    char corrupt_name[GT_FILE_NAME_MAX];
    strcpy(corrupt_name, segment_name);   /* fits: checked by the caller */
    strcat(corrupt_name, CORRUPT_SUFFIX);

    if (ops->rename(ctx, segment_name, corrupt_name) != GT_OK
        || ops->fsync_dir(ctx) != GT_OK) {
        return GT_ERR_IO;
    }
    return GT_OK;
}

/* One exit path: the handle is closed and the buffer freed exactly once,
 * whatever happened in between. */
gt_status gt_storage_recover_ops(const gt_file_ops *ops, void *ctx,
                                 const char *segment_name, gt_recover_report *report_out)
{
    if (ops == NULL || segment_name == NULL || report_out == NULL
        || strlen(segment_name) + strlen(CORRUPT_SUFFIX) >= GT_FILE_NAME_MAX) {
        return GT_ERR_INVAL;
    }

    int handle = -1;
    if (ops->open(ctx, segment_name, 0, &handle) != GT_OK) {
        return GT_ERR_IO;
    }

    uint8_t *bytes = NULL;
    size_t len = 0;
    gt_status status = read_whole_file(ops, ctx, handle, &bytes, &len);

    gt_scan_result scan;
    memset(&scan, 0, sizeof scan);   /* header_ok 0: an empty file has no header */
    if (status == GT_OK && bytes != NULL) {
        status = gt_recover_scan(bytes, len, &scan);
    }

    gt_recover_report report;
    memset(&report, 0, sizeof report);
    if (status == GT_OK && scan.header_ok && scan.good_len < len) {
        /* A damaged tail: cut it, and fsync so the cut survives too. */
        if (ops->ftruncate(ctx, handle, scan.good_len) != GT_OK
            || ops->fsync(ctx, handle) != GT_OK) {
            status = GT_ERR_IO;
        }
        report.outcome = GT_RECOVER_TRUNCATED;
        report.bytes_removed = len - scan.good_len;
        report.records_removed = scan.records_removed;
    } else {
        report.outcome = scan.header_ok ? GT_RECOVER_CLEAN : GT_RECOVER_RENAMED;
        report.bytes_removed = scan.header_ok ? 0 : len;
    }
    report.records_kept = scan.records_kept;
    report.has_records = scan.has_records;
    report.last_seq = scan.last_seq;

    ops->close(ctx, handle);
    free(bytes);

    if (status == GT_OK && report.outcome == GT_RECOVER_RENAMED) {
        status = rename_corrupt(ops, ctx, segment_name);
    }
    if (status == GT_OK) {
        *report_out = report;
    }
    return status;
}
