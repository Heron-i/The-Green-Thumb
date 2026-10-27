/* The offline log verifier (see log_verify.h).
 *
 * Purpose: give every fault test (kill -9, SIGTERM, power pull) one
 * yes/no answer: is the log on disk exactly what storage promises, every
 * record whole and checked, seq strictly increasing, and every hole in the
 * numbering explained by a GAP? It reads segments through the file seam,
 * oldest first, and stops at the first problem.
 *
 * Read-only: it opens, sizes and reads, and never writes, cuts, renames
 * or deletes anything, so running it can't change the evidence. */
#include "log_verify.h"

#include <stdlib.h>
#include <string.h>

#include "log_record.h"
#include "storage.h"

#define CORRUPT_SUFFIX     ".corrupt"
#define CORRUPT_SUFFIX_LEN 8

/* Where the stream has got to: the last good record, if any. */
typedef struct {
    int       has_prev;
    gt_record prev;
} stream_state;

/* Record the first failure in the report. */
static void fail_at(gt_verify_report *report, const char *failure, const char *segment,
                    uint64_t offset)
{
    report->ok = 0;
    report->failure = failure;
    memcpy(report->failed_segment, segment, GT_FILE_NAME_MAX);
    report->failed_offset = offset;
}

/* Count the *.corrupt files in the directory: recovery's set-aside
 * segments, reported but not failed. */
static gt_status count_corrupt(const gt_file_ops *ops, void *ctx, size_t *count_out)
{
    char names[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX];
    size_t name_count = 0;
    if (ops->list(ctx, names, &name_count) != GT_OK) {
        return GT_ERR_IO;
    }
    size_t corrupt = 0;
    for (size_t name_index = 0; name_index < name_count; name_index++) {
        size_t name_len = strlen(names[name_index]);
        if (name_len > CORRUPT_SUFFIX_LEN
            && strcmp(&names[name_index][name_len - CORRUPT_SUFFIX_LEN], CORRUPT_SUFFIX) == 0) {
            corrupt++;
        }
    }
    *count_out = corrupt;
    return GT_OK;
}

/* Read the whole segment `name` into a new malloc'd buffer (NULL for an
 * empty file); the caller frees it. The file is closed before returning. */
static gt_status read_segment(const gt_file_ops *ops, void *ctx, const char *name,
                              uint8_t **bytes_out, size_t *len_out)
{
    int handle = -1;
    if (ops->open(ctx, name, 0, &handle) != GT_OK) {
        return GT_ERR_IO;
    }
    gt_status status = GT_OK;
    uint64_t file_size = 0;
    uint8_t *bytes = NULL;
    if (ops->size(ctx, handle, &file_size) != GT_OK) {
        status = GT_ERR_IO;
    } else if ((uint64_t)(size_t)file_size != file_size) {
        status = GT_ERR_NOMEM;   /* too big to hold in memory at all */
    } else if (file_size > 0) {
        bytes = malloc((size_t)file_size);
        if (bytes == NULL) {
            status = GT_ERR_NOMEM;
        } else if (ops->pread(ctx, handle, 0, bytes, (size_t)file_size) != GT_OK) {
            status = GT_ERR_IO;
        }
    }
    ops->close(ctx, handle);
    if (status != GT_OK) {
        free(bytes);
        return status;
    }
    *bytes_out = bytes;
    *len_out = (size_t)file_size;
    return GT_OK;
}

/* Check one record against the one before it in the stream. Returns NULL
 * if it fits, or the failure name. The only allowed jump is right after a
 * GAP, to GAP.seq + GAP.value: append's rec(s), GAP(s + 1, v),
 * rec(s + 1 + v). The sum is done in 64 bits so it can't wrap. */
static const char *check_order(const stream_state *stream, const gt_record *record)
{
    if (!stream->has_prev) {
        return NULL;
    }
    const gt_record *prev = &stream->prev;
    if (record->seq <= prev->seq) {
        return GT_VERIFY_SEQ_BACKWARDS;
    }
    int next_in_line = record->seq == prev->seq + 1;
    int covered_by_gap = prev->type == GT_REC_GAP
                         && (uint64_t)record->seq == (uint64_t)prev->seq + prev->value;
    return next_in_line || covered_by_gap ? NULL : GT_VERIFY_SEQ_JUMP_UNCOVERED;
}

/* Check one segment's bytes, continuing the stream, and count what it
 * holds. Stops at (and reports) the first problem. */
static void check_segment(const char *name, const uint8_t *bytes, size_t len,
                          stream_state *stream, gt_verify_report *report)
{
    gt_seg_header header;
    if (len < GT_SEG_HEADER_SZ || gt_seg_header_decode(bytes, &header) != GT_OK) {
        fail_at(report, GT_VERIFY_BAD_HEADER, name, 0);
        return;
    }
    size_t offset = GT_SEG_HEADER_SZ;
    /* invariant: bytes [0, offset) are a good header and good records, all
     * in order with what came before */
    while (offset < len) {
        gt_record record;
        if (len - offset < GT_REC_SIZE) {
            fail_at(report, GT_VERIFY_PARTIAL_TAIL, name, offset);
            return;
        }
        if (gt_record_decode(&bytes[offset], &record) != GT_OK) {
            fail_at(report, GT_VERIFY_BAD_RECORD, name, offset);
            return;
        }
        const char *order_failure = check_order(stream, &record);
        if (order_failure != NULL) {
            fail_at(report, order_failure, name, offset);
            return;
        }
        if (record.type == GT_REC_GAP) {
            report->gap_records++;
            report->gap_readings += record.value;
        } else {
            report->data_records++;
        }
        if (!report->has_records) {
            report->has_records = 1;
            report->first_seq = record.seq;
        }
        report->last_seq = record.seq;
        stream->has_prev = 1;
        stream->prev = record;
        offset += GT_REC_SIZE;
    }
}

gt_status gt_verify_dir_ops(const gt_file_ops *ops, void *ctx, gt_verify_report *report_out)
{
    if (ops == NULL || report_out == NULL) {
        return GT_ERR_INVAL;
    }
    gt_verify_report report;
    memset(&report, 0, sizeof report);
    report.ok = 1;

    char segments[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX];
    size_t segment_count = 0;
    int marker_present = 0;   /* not checked: the marker says nothing about the records */
    gt_status status = count_corrupt(ops, ctx, &report.corrupt_files);
    if (status == GT_OK) {
        status = gt_storage_list_segments(ops, ctx, segments, &segment_count, &marker_present);
    }

    stream_state stream;
    memset(&stream, 0, sizeof stream);
    /* invariant: segments[0 .. segment_index - 1] all passed, in order */
    for (size_t segment_index = 0; status == GT_OK && report.ok && segment_index < segment_count;
         segment_index++) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        status = read_segment(ops, ctx, segments[segment_index], &bytes, &len);
        if (status == GT_OK) {
            report.segments++;
            check_segment(segments[segment_index], bytes, len, &stream, &report);
        }
        free(bytes);
    }
    if (status == GT_OK) {
        *report_out = report;
    }
    return status;
}

/* A passing report covers every seq in first_seq..last_seq, so the sent
 * seqs accounted for are exactly the overlap of that range with A..B, and
 * lost is the rest. All counts are 64-bit, so 0..UINT32_MAX can't wrap. */
gt_status gt_verify_sent(const gt_verify_report *report, uint32_t sent_first,
                         uint32_t sent_last, gt_verify_sent_result *result_out)
{
    if (report == NULL || result_out == NULL || sent_first > sent_last) {
        return GT_ERR_INVAL;
    }
    gt_verify_sent_result result = {
        .ok = 0, .sent = (uint64_t)sent_last - sent_first + 1, .lost = 0, .failure = NULL,
    };
    if (!report->ok) {
        result.failure = GT_VERIFY_LOG_FAILED;
        *result_out = result;
        return GT_OK;
    }
    uint64_t held = 0;
    if (report->has_records) {
        uint32_t overlap_first = report->first_seq > sent_first ? report->first_seq : sent_first;
        uint32_t overlap_last = report->last_seq < sent_last ? report->last_seq : sent_last;
        if (overlap_first <= overlap_last) {
            held = (uint64_t)overlap_last - overlap_first + 1;
        }
    }
    result.lost = result.sent - held;
    if (report->has_records && report->last_seq > sent_last) {
        result.failure = GT_VERIFY_SEQ_NOT_SENT;
    } else if (result.lost > 0) {
        result.failure = GT_VERIFY_SENT_SEQ_LOST;
    } else {
        result.ok = 1;
    }
    *result_out = result;
    return GT_OK;
}
