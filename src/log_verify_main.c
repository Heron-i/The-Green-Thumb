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
 * and on a failure a last line, e.g.
 *   failure=seq_jump_uncovered segment=log-20261026.bin offset=64
 *
 * With --sent A..B (the first and last seq one hub run sent), a log that
 * passed is also compared with that range (gt_verify_sent, PF-12): every
 * sent seq must be stored or covered by a GAP, and nothing after B may be
 * in the log. Seqs before A are earlier runs and are allowed. It adds
 *   sent=10 lost=0 sent_first=10 sent_last=19
 * and, if that check fails, e.g. "failure=sent_seq_lost".
 * Exit status: 0 PASS, 1 FAIL, 2 the directory could not be read or the
 * arguments are wrong.
 *
 * Usage: log_verify [--sent A..B] /var/lib/plantpi */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log_verify.h"
#include "storage_io.h"

/* Parse one whole decimal seq (0..UINT32_MAX) from `text`, which must end
 * at `end` (exclusive). strtoull ("string to unsigned long long") is used
 * rather than strtoul because unsigned long is only 32 bits on 32-bit Pi
 * OS, where an out-of-range value would be hard to tell apart. Returns 1
 * on success. */
static int parse_seq(const char *text, const char *end, uint32_t *seq_out)
{
    if (text == end || *text < '0' || *text > '9') {
        return 0;   /* empty, or a sign/space strtoull would accept */
    }
    char *parsed_end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &parsed_end, 10);
    if (errno != 0 || parsed_end != end || value > UINT32_MAX) {
        return 0;
    }
    *seq_out = (uint32_t)value;
    return 1;
}

/* Parse "A..B", the sender's first and last seq. Returns 1 on success
 * with A <= B; *first_out and *last_out are untouched on failure. */
static int parse_sent_range(const char *text, uint32_t *first_out, uint32_t *last_out)
{
    const char *dots = strstr(text, "..");
    uint32_t first = 0, last = 0;
    if (dots == NULL || !parse_seq(text, dots, &first)
        || !parse_seq(dots + 2, dots + 2 + strlen(dots + 2), &last) || first > last) {
        return 0;
    }
    *first_out = first;
    *last_out = last;
    return 1;
}

int main(int argc, char **argv)
{
    int has_sent = 0;
    uint32_t sent_first = 0, sent_last = 0;
    if (argc == 4 && strcmp(argv[1], "--sent") == 0) {
        has_sent = parse_sent_range(argv[2], &sent_first, &sent_last);
    }
    if (argc != 2 && !has_sent) {
        fprintf(stderr, "usage: %s [--sent A..B] /var/lib/plantpi   (run with storage stopped)\n",
                argv[0]);
        return 2;
    }
    const char *log_dir = argv[argc - 1];
    const gt_file_ops *ops = NULL;
    void *ctx = NULL;
    if (gt_file_ops_linux(log_dir, &ops, &ctx) != GT_OK) {
        fprintf(stderr, "log_verify: can't open %s\n", log_dir);
        return 2;
    }
    gt_verify_report report;
    gt_status status = gt_verify_dir_ops(ops, ctx, &report);
    ops->destroy(ctx);
    if (status != GT_OK) {
        fprintf(stderr, "log_verify: can't read %s (status %d)\n", log_dir, (int)status);
        return 2;
    }
    /* With --sent, a log that passed is also compared with the sender's
     * range (PF-12); a log that failed already says why below. */
    gt_verify_sent_result sent = { .ok = 1, .sent = 0, .lost = 0, .failure = NULL };
    int check_sent = has_sent && report.ok;
    if (check_sent) {
        gt_verify_sent(&report, sent_first, sent_last, &sent);   /* args checked above */
    }
    int passed = report.ok && sent.ok;

    printf("log_verify: %s\n", passed ? "PASS" : "FAIL");
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
    if (check_sent) {
        printf("sent=%llu lost=%llu sent_first=%lu sent_last=%lu\n",
               (unsigned long long)sent.sent, (unsigned long long)sent.lost,
               (unsigned long)sent_first, (unsigned long)sent_last);
    }
    if (!report.ok) {
        printf("failure=%s segment=%s offset=%llu\n", report.failure, report.failed_segment,
               (unsigned long long)report.failed_offset);
    } else if (!sent.ok) {
        printf("failure=%s\n", sent.failure);
    }
    return passed ? 0 : 1;
}
