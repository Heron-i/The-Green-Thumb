#ifndef HUB_H
#define HUB_H

#include <stdint.h>
#include "ipc.h"

typedef struct gt_hub gt_hub;

gt_hub   *gt_hub_create(void);
/* Validate a message, stamp seq + time, and fill `out` for forwarding. */
gt_status gt_hub_accept(gt_hub *h, const gt_msg *in, uint64_t now_us, gt_msg *out);
/* Per-sensor last-seen check; returns number of timed-out sensors. */
int       gt_hub_check_timeouts(gt_hub *h, uint64_t now_us);
void      gt_hub_destroy(gt_hub *h);

#endif
