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

/* Comparison with what the sender sent (DESIGN.md Evaluation plan: lost
 * readings = seqs the hub sent - seqs committed - seqs covered by a GAP;
 * PF-12). Within one hub run the sent seqs are one unbroken range A..B: a
 * seq is used only when send() succeeds, and a running hub's counter is
 * always ahead of storage's reply. A passing report already guarantees
 * every seq from first_seq to last_seq is stored or covered by a GAP, so
 * only the two ranges need comparing:
 *   - seqs before A are allowed: they are earlier hub runs;
 *   - a seq after B was never sent by this run: seq_not_sent;
 *   - every sent seq outside first_seq..last_seq is lost: sent_seq_lost.
 * Run it after the hub has stopped, before a new run adds to the log. */
#define GT_VERIFY_LOG_FAILED    "log_failed"     /* the report itself did not pass */
#define GT_VERIFY_SEQ_NOT_SENT  "seq_not_sent"   /* the log goes past B */
#define GT_VERIFY_SENT_SEQ_LOST "sent_seq_lost"  /* some of A..B is missing */

typedef struct {
    int         ok;        /* 1: every sent seq is accounted for, nothing extra */
    uint64_t    sent;      /* B - A + 1 (64 bits: 0..UINT32_MAX is 2^32 seqs) */
    uint64_t    lost;      /* sent seqs neither stored nor covered by a GAP */
    const char *failure;   /* one of the three above, or NULL when ok */
} gt_verify_sent_result;

/* Compare `report` (from gt_verify_dir_ops) with the sent range
 * sent_first..sent_last. When the report did not pass, the result fails
 * with log_failed and lost is not computed (0).
 *   GT_OK         the comparison ran; *result_out says pass or fail
 *   GT_ERR_INVAL  NULL args, or sent_first > sent_last
 * *result_out is untouched unless GT_OK. */
gt_status gt_verify_sent(const gt_verify_report *report, uint32_t sent_first,
                         uint32_t sent_last, gt_verify_sent_result *result_out);

#endif
