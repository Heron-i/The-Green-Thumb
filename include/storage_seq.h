#ifndef STORAGE_SEQ_H
#define STORAGE_SEQ_H

#include <stdint.h>
#include "ipc.h"
#include "log_record.h"

/* Sequence guard: what storage does with each incoming record's seq
 * (DESIGN.md failure-mode table, Hub "seq starts over" row and Storage
 * "Killed ... with an unsynced batch" row).
 *
 * The hub stamps every record it forwards with the next seq, so in the
 * log seq goes up by exactly 1 from record to record. Storage compares
 * each incoming seq with the last one it *accepted* (which may still be
 * in the pending buffer, not yet on disk), so a duplicate is caught even
 * before its twin is committed:
 *   - lower or equal: a duplicate or an old record. Rejected (seq_rejected).
 *   - exactly one more: accepted.
 *   - more than one more: records went missing between the hub and
 *     storage (for example storage was killed with records still in the
 *     socket). Storage first writes one GAP record for the missing range
 *     (recovery_gap), then accepts the record, so no loss is silent.
 * Wrapping past UINT32_MAX (about 136 years at the real rate) is not
 * handled.
 *
 * Pure logic: no files, no state. The caller keeps last_accepted. */
typedef enum {
    GT_SEQ_ACCEPT,   /* store it */
    GT_SEQ_REJECT,   /* drop it and log seq_rejected */
    GT_SEQ_GAP       /* store a gt_seq_gap_record first, then store it */
} gt_seq_verdict;

/* Judge `incoming` against `last_accepted`. has_last is 0 when storage has
 * accepted nothing yet (an empty log); then any seq is accepted and
 * last_accepted is ignored. */
gt_seq_verdict gt_seq_check(uint32_t last_accepted, int has_last, uint32_t incoming);

/* Fill *gap_out with the GAP record for the seqs missing between
 * last_accepted and incoming->seq:
 *   type GAP, sensor_id GT_SENSOR_ALL (storage can't know which sensor
 *   the missing records came from), seq last_accepted + 1, value the
 *   number of missing seqs, and timestamp_us = last_accepted_us, the
 *   last accepted record's time.
 * The missing records' own times are lost with them, so the gap is
 * stamped with the earliest moment it could have started: right after
 * the last record storage has. DESIGN.md: a GAP's time is the gap's
 * start, and the gap ends at the next record's time (PF-07).
 * GT_ERR_INVAL for NULL args, or when incoming->seq is not more than
 * last_accepted + 1 (there is no gap); *gap_out is untouched on failure. */
gt_status gt_seq_gap_record(uint32_t last_accepted, uint64_t last_accepted_us,
                            const gt_record *incoming, gt_record *gap_out);

#endif
