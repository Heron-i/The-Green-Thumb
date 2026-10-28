/* The 8 B seq_resume reply, storage -> hub (see storage_proto.h).
 *
 * Purpose: turns a gt_resume into the exact bytes storage sends a newly
 * connected hub, and back, so the hub can carry on numbering after the
 * last seq storage holds. As in log_record.c and ipc_msg.c, fields are
 * written one byte at a time, little-endian, never by copying the struct.
 *
 * Pure byte packing: no sockets. */
#include "storage_proto.h"

#include <stddef.h>

gt_status gt_resume_encode(const gt_resume *resume, uint8_t out[GT_RESUME_SIZE])
{
    if (resume == NULL || out == NULL) {
        return GT_ERR_INVAL;
    }
    uint32_t seq = resume->has_last ? resume->seq : 0;   /* no seq without has_last */

    out[0] = GT_RESUME_MAGIC;
    out[1] = GT_RESUME_VERSION;
    out[2] = GT_RESUME_TYPE;
    out[3] = resume->has_last ? 1 : 0;
    out[4] = (uint8_t)(seq);
    out[5] = (uint8_t)(seq >> 8);
    out[6] = (uint8_t)(seq >> 16);
    out[7] = (uint8_t)(seq >> 24);
    return GT_OK;
}

/* 1 if the reply starts with the seq_resume magic, version and type. */
static int resume_header_ok(const uint8_t in[GT_RESUME_SIZE])
{
    return in[0] == GT_RESUME_MAGIC && in[1] == GT_RESUME_VERSION && in[2] == GT_RESUME_TYPE;
}

/* 1 if has_last is 1, or is 0 with seq 0 (no seq without has_last). */
static int resume_has_last_ok(uint8_t has_last, uint32_t seq)
{
    return has_last == 1 || (has_last == 0 && seq == 0);
}

/* Every check runs before `resume` is written, so a rejected reply leaves
 * it exactly as it was. Each byte is widened to uint32_t before shifting,
 * so a shift by 24 can't overflow an int. */
gt_status gt_resume_decode(const uint8_t in[GT_RESUME_SIZE], gt_resume *resume)
{
    if (in == NULL || resume == NULL) {
        return GT_ERR_INVAL;
    }
    uint32_t seq = (uint32_t)in[4] | ((uint32_t)in[5] << 8)
                 | ((uint32_t)in[6] << 16) | ((uint32_t)in[7] << 24);
    if (!resume_header_ok(in) || !resume_has_last_ok(in[3], seq)) {
        return GT_ERR_CORRUPT;
    }
    resume->has_last = in[3];
    resume->seq = seq;
    return GT_OK;
}

/* after_reply is the first seq storage doesn't hold yet. The UINT32_MAX
 * check comes before the + 1, so it can never wrap around to 0. */
gt_status gt_next_seq(int has_own, uint32_t own_next, const gt_resume *resume,
                      uint32_t *next_out)
{
    if (resume == NULL || next_out == NULL) {
        return GT_ERR_INVAL;
    }
    if (resume->has_last && resume->seq == UINT32_MAX) {
        return GT_ERR_FULL;
    }
    uint32_t after_reply = resume->has_last ? resume->seq + 1 : 0;
    *next_out = (has_own && own_next > after_reply) ? own_next : after_reply;
    return GT_OK;
}
