#ifndef LOG_VERIFY_H
#define LOG_VERIFY_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"
#include "storage_io.h"

/* The offline log verifier (DESIGN.md Evaluation plan, Test tooling:
 * "reads the log offline and checks every record's CRC, that seq strictly
 * increases, and that every missing seq is covered by a GAP record").
 * It is the pass/fail check behind every kill -9, SIGTERM and power-pull
 * test, so it only reads: it never truncates, renames or writes.
 *
 * The log is checked as one stream, segment after segment, oldest first:
 *   - each segment: a valid 16 B header, then only whole records that pass
 *     gt_record_decode (a partial record at the end is a failure: run the
 *     verifier with storage stopped, or it may catch a batch mid-write);
 *   - across all segments, after a record with seq s the next must be
 *     s + 1, unless the previous record is a GAP and the next seq equals
 *     GAP.seq + GAP.value. That is the shape append writes when seqs went
 *     missing: rec(s), GAP(s + 1, v), rec(s + 1 + v).
 * It stops at the first failure and says where. */

/* Failure names in gt_verify_report.failure (NULL when ok). */
#define GT_VERIFY_BAD_HEADER          "bad_header"
#define GT_VERIFY_BAD_RECORD          "bad_record"
#define GT_VERIFY_PARTIAL_TAIL        "partial_tail"
#define GT_VERIFY_SEQ_BACKWARDS       "seq_backwards"
#define GT_VERIFY_SEQ_JUMP_UNCOVERED  "seq_jump_uncovered"

typedef struct {
    int         ok;               /* 1: every check passed */
    size_t      segments;         /* segments read (up to the failure) */
    size_t      corrupt_files;    /* *.corrupt files present: recovery set data aside */
    uint64_t    data_records;
    uint64_t    gap_records;
    uint64_t    gap_readings;     /* sum of every GAP's value: readings marked missing */
    int         has_records;      /* 1 if first_seq / last_seq mean something */
    uint32_t    first_seq;
    uint32_t    last_seq;         /* last good record before any failure */
    const char *failure;          /* one of GT_VERIFY_*, or NULL */
    char        failed_segment[GT_FILE_NAME_MAX];   /* "" when ok */
    uint64_t    failed_offset;    /* byte offset in failed_segment */
} gt_verify_report;

/* Verify every segment in the directory behind ops/ctx. The caller keeps
 * ctx (it is not destroyed).
 *   GT_OK         the check ran; *report_out says pass or fail
 *   GT_ERR_IO     the directory or a segment could not be read
 *   GT_ERR_NOMEM  a segment could not be read into memory
 *   GT_ERR_INVAL  NULL args
 * *report_out is untouched unless GT_OK. */
gt_status gt_verify_dir_ops(const gt_file_ops *ops, void *ctx, gt_verify_report *report_out);

#endif
