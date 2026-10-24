#ifndef HUB_H
#define HUB_H

#include <stdint.h>
#include "ipc.h"
#include "log_record.h"

typedef struct gt_hub gt_hub;

gt_hub   *gt_hub_create(void);
/* Validate a sensor message, stamp seq + now_us, and fill the 24 B record
 * `out` that is forwarded to storage. */
gt_status gt_hub_accept(gt_hub *h, const gt_msg *in, uint64_t now_us, gt_record *out);
/* Per-sensor last-seen check; returns number of timed-out sensors. */
int       gt_hub_check_timeouts(gt_hub *h, uint64_t now_us);
void      gt_hub_destroy(gt_hub *h);

#endif
