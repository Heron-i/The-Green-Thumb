/* Unit tests for frames arriving on storage.sock (src/storage.c):
 * gt_storage_rx and gt_storage_resume.
 *
 * Purpose: prove storage keeps the hub's connection for everything that is
 * the hub's normal business (an accepted record, a duplicate seq, a full
 * buffer, a gap) and closes it only for a malformed frame (DESIGN.md
 * failure-mode table, Storage "Malformed message on storage.sock": discard
 * it and close the connection; the hub reconnects), or a disconnect. And
 * that seq_resume reports the last ACCEPTED seq, including records still
 * pending, so a reconnecting hub never reuses a seq storage holds. Frames
 * are passed in as recv() would return them; uses tests/fake_file.h, no
 * sockets. Run under make test / asan / memcheck. Implements SRX-01
 * through SRX-09.
 *
 * What these tests cannot prove: the real socket, accepting the hub and
 * sending the reply on connect (storage_main, Mechanism D Step 12d). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "fake_file.h"
#include "log_record.h"
#include "storage.h"

#define TODAY_US      ((uint64_t)(1792972800u + 3600u) * 1000000u)   /* 2026-10-26 01:00 UTC */
#define TODAY_NAME    "log-20261026.bin"
#define TEST_CAPACITY 8
#define RX_BUF_SIZE   (GT_REC_SIZE + 1)   /* one extra byte, as the caller's recv buffer */

static fake_fs fs;   /* static: a fake_fs is about 70 KB (see fake_file.h) */

/* Encode a light DATA record with `seq` into the start of `buf`. */
static void make_frame(uint8_t buf[RX_BUF_SIZE], uint32_t seq)
{
    const gt_record record = {
        .type = GT_REC_DATA, .sensor_id = GT_SENSOR_LIGHT,
        .seq = seq, .timestamp_us = 3000 + seq, .value = 60 + seq,
    };
    memset(buf, 0, RX_BUF_SIZE);
    CHECK_EQ(gt_record_encode(&record, buf), GT_OK);
}

/* Open storage on a fresh fake, after putting today's segment with records
 * seq 1 .. last_on_disk in it (none if last_on_disk is 0). */
static gt_storage *open_storage(uint32_t last_on_disk)
{
    const gt_file_ops *ops = fake_fs_ops(&fs);
    if (last_on_disk > 0) {
        uint8_t segment[GT_SEG_HEADER_SZ + 8 * GT_REC_SIZE];
        uint8_t frame[RX_BUF_SIZE];
        const gt_seg_header header = { .created_at_us = 1000 };
        CHECK_EQ(gt_seg_header_encode(&header, segment), GT_OK);
        /* invariant: records 1 .. seq - 1 are encoded after the header */
        for (uint32_t seq = 1; seq <= last_on_disk; seq++) {
            make_frame(frame, seq);
            memcpy(&segment[GT_SEG_HEADER_SZ + (seq - 1) * GT_REC_SIZE], frame, GT_REC_SIZE);
        }
        CHECK_EQ(fake_fs_put(&fs, TODAY_NAME, segment,
                             GT_SEG_HEADER_SZ + last_on_disk * GT_REC_SIZE), GT_OK);
    }
    gt_storage *storage = NULL;
    CHECK_EQ(gt_storage_open_ops(ops, &fs, TODAY_US, TEST_CAPACITY, &storage, NULL), GT_OK);
    return storage;
}

/* Hand one frame to storage and check the action and the events. */
static void rx_expect(gt_storage *storage, const uint8_t *buf, long len,
                      gt_rx_action expected_action, unsigned expected_events)
{
    unsigned events = 0xFFFFu;   /* rx must reset it */
    CHECK_EQ(gt_storage_rx(storage, buf, len, 0, &events), expected_action);
    CHECK_EQ(events, expected_events);
}

/* Records waiting to be committed. */
static size_t pending_count(const gt_storage *storage)
{
    gt_storage_stats stats;
    CHECK_EQ(gt_storage_read_stats(storage, &stats), GT_OK);
    return stats.pending;
}

/* ---- SRX-01: a good frame is kept ---- */
static void test_srx01_good_frame_kept(void)
{
    gt_storage *storage = open_storage(0);
    uint8_t frame[RX_BUF_SIZE];
    make_frame(frame, 1);

    rx_expect(storage, frame, GT_REC_SIZE, GT_RX_KEEP, 0);
    CHECK_EQ(pending_count(storage), 1);
    gt_storage_close(storage);
}

/* ---- SRX-02: a wrong-length frame closes ----
 * 23 B (short), 25 B (cut off by SEQPACKET, seen as one byte too many)
 * and 16 B (a sensor message sent to the wrong socket). Nothing stored. */
static void test_srx02_wrong_length_closes(void)
{
    gt_storage *storage = open_storage(0);
    uint8_t frame[RX_BUF_SIZE];
    make_frame(frame, 1);
    const long wrong_lengths[] = { GT_REC_SIZE - 1, GT_REC_SIZE + 1, 16 };

    for (size_t length_index = 0; length_index < 3; length_index++) {
        rx_expect(storage, frame, wrong_lengths[length_index], GT_RX_CLOSE,
                  GT_STORAGE_EV_BAD_MESSAGE);
    }
    CHECK_EQ(pending_count(storage), 0);
    gt_storage_close(storage);
}

/* ---- SRX-03: a disconnect closes quietly ----
 * recv() returning 0 (the hub closed) or -1 (an error) is a disconnect,
 * not a bad message: CLOSE with no event. */
static void test_srx03_disconnect_quiet(void)
{
    gt_storage *storage = open_storage(0);
    uint8_t frame[RX_BUF_SIZE];
    make_frame(frame, 1);

    rx_expect(storage, frame, 0, GT_RX_CLOSE, 0);
    rx_expect(storage, frame, -1, GT_RX_CLOSE, 0);
    gt_storage_close(storage);
}

/* ---- SRX-04: bad bytes close ----
 * A flipped bit fails the CRC: discard and close, so the hub resyncs. */
static void test_srx04_bad_bytes_close(void)
{
    gt_storage *storage = open_storage(0);
    uint8_t frame[RX_BUF_SIZE];
    make_frame(frame, 1);
    frame[16] ^= 0x01;

    rx_expect(storage, frame, GT_REC_SIZE, GT_RX_CLOSE, GT_STORAGE_EV_BAD_MESSAGE);
    CHECK_EQ(pending_count(storage), 0);
    gt_storage_close(storage);
}

/* ---- SRX-05: a duplicate keeps the connection ----
 * Refused (SEQ_REJECTED), but the hub is healthy: no reason to close. */
static void test_srx05_duplicate_keeps(void)
{
    gt_storage *storage = open_storage(0);
    uint8_t frame[RX_BUF_SIZE];
    make_frame(frame, 1);

    rx_expect(storage, frame, GT_REC_SIZE, GT_RX_KEEP, 0);
    rx_expect(storage, frame, GT_REC_SIZE, GT_RX_KEEP, GT_STORAGE_EV_SEQ_REJECTED);
    CHECK_EQ(pending_count(storage), 1);
    gt_storage_close(storage);
}

/* ---- SRX-06: a full buffer keeps the connection ----
 * Storage keeps reading its socket while full (DESIGN.md: "so the hub and
 * sensors never block"): the 9th record is refused with QUEUE_FULL, and
 * the connection stays. */
static void test_srx06_full_keeps(void)
{
    gt_storage *storage = open_storage(0);
    uint8_t frame[RX_BUF_SIZE];

    /* invariant: records 1 .. seq - 1 were kept */
    for (uint32_t seq = 1; seq <= TEST_CAPACITY; seq++) {
        make_frame(frame, seq);
        CHECK_EQ(gt_storage_rx(storage, frame, GT_REC_SIZE, 0, NULL), GT_RX_KEEP);
    }
    make_frame(frame, TEST_CAPACITY + 1);
    rx_expect(storage, frame, GT_REC_SIZE, GT_RX_KEEP, GT_STORAGE_EV_QUEUE_FULL);
    gt_storage_close(storage);
}

/* ---- SRX-07: a skip passes on the gap event ---- */
static void test_srx07_skip_passes_gap(void)
{
    gt_storage *storage = open_storage(0);
    uint8_t frame[RX_BUF_SIZE];

    make_frame(frame, 1);
    rx_expect(storage, frame, GT_REC_SIZE, GT_RX_KEEP, 0);
    make_frame(frame, 4);
    rx_expect(storage, frame, GT_REC_SIZE, GT_RX_KEEP, GT_STORAGE_EV_RECOVERY_GAP);
    CHECK_EQ(pending_count(storage), 3);
    gt_storage_close(storage);
}

/* ---- SRX-08: resume reports the last accepted seq ----
 * Empty log: has_last 0. Log ending at 7: 7. After record 8 is accepted
 * but not yet flushed: 8, not the committed 7, so a reconnecting hub
 * carries on at 9. The reply also encodes. */
static void test_srx08_resume_last_accepted(void)
{
    gt_resume resume;
    uint8_t frame[RX_BUF_SIZE];
    uint8_t wire[GT_RESUME_SIZE];

    gt_storage *storage = open_storage(0);
    CHECK_EQ(gt_storage_resume(storage, &resume), GT_OK);
    CHECK_EQ(resume.has_last, 0);
    CHECK_EQ(resume.seq, 0);
    gt_storage_close(storage);

    storage = open_storage(7);
    CHECK_EQ(gt_storage_resume(storage, &resume), GT_OK);
    CHECK_EQ(resume.has_last, 1);
    CHECK_EQ(resume.seq, 7);

    make_frame(frame, 8);
    CHECK_EQ(gt_storage_rx(storage, frame, GT_REC_SIZE, 0, NULL), GT_RX_KEEP);
    CHECK_EQ(gt_storage_resume(storage, &resume), GT_OK);
    CHECK_EQ(resume.seq, 8);
    CHECK_EQ(gt_resume_encode(&resume, wire), GT_OK);
    gt_storage_close(storage);
}

/* ---- SRX-09: NULL arguments are handled ----
 * rx closes with no event; resume is GT_ERR_INVAL. */
static void test_srx09_null_args(void)
{
    gt_storage *storage = open_storage(0);
    uint8_t frame[RX_BUF_SIZE];
    gt_resume resume;
    make_frame(frame, 1);

    rx_expect(NULL, frame, GT_REC_SIZE, GT_RX_CLOSE, 0);
    rx_expect(storage, NULL, GT_REC_SIZE, GT_RX_CLOSE, 0);
    CHECK_EQ(gt_storage_resume(NULL, &resume), GT_ERR_INVAL);
    CHECK_EQ(gt_storage_resume(storage, NULL), GT_ERR_INVAL);
    gt_storage_close(storage);
}

static const test_case_t tests[] = {
    {"SRX-01", "a good frame is kept", test_srx01_good_frame_kept},
    {"SRX-02", "a wrong-length frame closes", test_srx02_wrong_length_closes},
    {"SRX-03", "a disconnect closes quietly", test_srx03_disconnect_quiet},
    {"SRX-04", "bad bytes close", test_srx04_bad_bytes_close},
    {"SRX-05", "a duplicate keeps the connection", test_srx05_duplicate_keeps},
    {"SRX-06", "a full buffer keeps the connection", test_srx06_full_keeps},
    {"SRX-07", "a skip passes on the gap event", test_srx07_skip_passes_gap},
    {"SRX-08", "resume reports the last accepted seq", test_srx08_resume_last_accepted},
    {"SRX-09", "NULL arguments are handled", test_srx09_null_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
