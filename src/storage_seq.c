/* Sequence guard: accept, reject or gap for each incoming seq (see
 * storage_seq.h).
 *
 * Purpose: keep seq strictly increasing in the log, which the recovery
 * scan relies on, and turn every hole in the hub's numbering into an
 * explicit GAP record instead of a silent loss.
 *
 * Pure logic: no files, no state. */
#include "storage_seq.h"

#include <stddef.h>

/* last_accepted < incoming is checked before the + 1, so last_accepted is
 * never UINT32_MAX when the + 1 runs and it can't wrap around to 0. */
gt_seq_verdict gt_seq_check(uint32_t last_accepted, int has_last, uint32_t incoming)
{
    if (!has_last) {
        return GT_SEQ_ACCEPT;
    }
    if (incoming <= last_accepted) {
        return GT_SEQ_REJECT;
    }
    if (incoming == last_accepted + 1) {
        return GT_SEQ_ACCEPT;
    }
    return GT_SEQ_GAP;
}

gt_status gt_seq_gap_record(uint32_t last_accepted, uint64_t last_accepted_us,
                            const gt_record *incoming, gt_record *gap_out)
{
    if (incoming == NULL || gap_out == NULL) {
        return GT_ERR_INVAL;
    }
    if (gt_seq_check(last_accepted, 1, incoming->seq) != GT_SEQ_GAP) {
        return GT_ERR_INVAL;
    }

    gap_out->type = GT_REC_GAP;
    gap_out->sensor_id = GT_SENSOR_ALL;
    gap_out->seq = last_accepted + 1;
    gap_out->timestamp_us = last_accepted_us;   /* the gap started after it (PF-07) */
    gap_out->value = incoming->seq - last_accepted - 1;   /* the seqs strictly between */
    return GT_OK;
}
