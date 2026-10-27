#ifndef STORAGE_PROTO_H
#define STORAGE_PROTO_H

#include <stdint.h>
#include "ipc.h"

/* storage -> hub reply on storage.sock: seq_resume (DESIGN.md failure-mode
 * table, Hub (re)connect row). Storage sends it once, as soon as the
 * hub connects. A freshly started hub carries on numbering after the last
 * seq storage already holds instead of starting over at 0 (which storage
 * would reject as old); a hub that is still running keeps its own counter
 * when that is ahead (gt_next_seq below).
 *
 * The seq is the last one storage ACCEPTED, which may still be pending,
 * not the last committed: if storage holds 41-60 unflushed and the hub
 * reconnects, replying 40 would make the hub reuse 41-60, and storage
 * would reject those readings as duplicates. Right after a storage
 * restart the two are the same, since unflushed records died with it.
 *
 * Wire layout, 8 B, little-endian, written byte by byte like the record:
 *   off 0  magic     GT_RESUME_MAGIC (distinct from 0xA6 and 0xA7)
 *   off 1  version   GT_RESUME_VERSION
 *   off 2  type      GT_RESUME_TYPE
 *   off 3  has_last  0: storage holds nothing yet (a fresh hub starts at 0;
 *                    a running hub keeps its counter);
 *                    1: seq is valid
 *   off 4  seq       u32; must be 0 when has_last is 0
 * No CRC: the local socket keeps each message whole, and the kernel does
 * not corrupt it in transit. The record's CRC guards the SD card. */
#define GT_RESUME_SIZE    8
#define GT_RESUME_MAGIC   0xA8
#define GT_RESUME_VERSION 1
#define GT_RESUME_TYPE    0x10

typedef struct {
    int      has_last;   /* 0 or 1 */
    uint32_t seq;        /* the last accepted seq; 0 when has_last is 0 */
} gt_resume;

/* Write `resume` as 8 bytes. has_last is written as 0 or 1 (any non-zero
 * value counts as 1), and seq as 0 when has_last is 0. GT_ERR_INVAL for
 * NULL args; `out` is untouched on failure. */
gt_status gt_resume_encode(const gt_resume *resume, uint8_t out[GT_RESUME_SIZE]);

/* Read 8 bytes. GT_ERR_CORRUPT for a bad magic, version or type, a
 * has_last other than 0 or 1, or a non-zero seq with has_last 0;
 * GT_ERR_INVAL for NULL args. `resume` is untouched on failure. */
gt_status gt_resume_decode(const uint8_t in[GT_RESUME_SIZE], gt_resume *resume);

/* Pick the sender's next seq after a seq_resume reply (PF-03):
 * max(own_next, resume->seq + 1). has_own is 0 for a sender that has not
 * sent anything yet in this process; own_next is then ignored and the
 * result is resume->seq + 1, or 0 when resume->has_last is 0. A sender
 * that is still running keeps its own counter, so seqs storage lost in a
 * kill -9 leave a hole that storage covers with a GAP (recovery_gap).
 * GT_ERR_INVAL for NULL args; GT_ERR_FULL when resume->seq is UINT32_MAX
 * (no next seq exists; wrapping isn't handled, see storage_seq.h).
 * *next_out is untouched on failure. */
gt_status gt_next_seq(int has_own, uint32_t own_next, const gt_resume *resume,
                      uint32_t *next_out);

#endif
