/* log_verify: check a storage log directory offline (DESIGN.md Evaluation
 * plan, Test tooling: the verifier).
 *
 * Purpose: one yes/no answer after every fault test (kill -9, SIGTERM,
 * power pull): is every record whole and checked, does seq strictly
 * increase across all segments, and is every hole in the numbering
 * explained by a GAP record? The checks are gt_verify_dir_ops
 * (src/log_verify.c, tested by tests/test_log_verify.c); this file only
 * opens the directory and prints the report. It only reads, so it can't
 * change the evidence. Run it with storage stopped: while storage is
 * writing, it may catch a batch half-written and report a partial tail.
 *
 * Output (also parsed by tools/storage_faults.sh):
 *   log_verify: PASS
 *   segments=2 corrupt_files=0 data=250 gaps=1 gap_readings=3 first_seq=0 last_seq=252
 * and on a failure a third line, e.g.
 *   failure=seq_jump_uncovered segment=log-20261026.bin offset=64
 * Exit status: 0 PASS, 1 FAIL, 2 the directory could not be read.
 *
 * Usage: log_verify /var/lib/plantpi */
#include <stdio.h>

#include "log_verify.h"
#include "storage_io.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s /var/lib/plantpi   (run with storage stopped)\n", argv[0]);
        return 2;
    }
    const gt_file_ops *ops = NULL;
    void *ctx = NULL;
    if (gt_file_ops_linux(argv[1], &ops, &ctx) != GT_OK) {
        fprintf(stderr, "log_verify: can't open %s\n", argv[1]);
        return 2;
    }
    gt_verify_report report;
    gt_status status = gt_verify_dir_ops(ops, ctx, &report);
    ops->destroy(ctx);
    if (status != GT_OK) {
        fprintf(stderr, "log_verify: can't read %s (status %d)\n", argv[1], (int)status);
        return 2;
    }

    printf("log_verify: %s\n", report.ok ? "PASS" : "FAIL");
    printf("segments=%lu corrupt_files=%lu data=%llu gaps=%llu gap_readings=%llu",
           (unsigned long)report.segments, (unsigned long)report.corrupt_files,
           (unsigned long long)report.data_records, (unsigned long long)report.gap_records,
           (unsigned long long)report.gap_readings);
    if (report.has_records) {
        printf(" first_seq=%lu last_seq=%lu\n", (unsigned long)report.first_seq,
               (unsigned long)report.last_seq);
    } else {
        printf(" first_seq=none last_seq=none\n");
    }
    if (!report.ok) {
        printf("failure=%s segment=%s offset=%llu\n", report.failure, report.failed_segment,
               (unsigned long long)report.failed_offset);
    }
    return report.ok ? 0 : 1;
}
