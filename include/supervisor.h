#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include <stdint.h>
#include "ipc.h"

typedef struct gt_supervisor gt_supervisor;

gt_supervisor *gt_sup_create(void);
/* Register a child by argv; started in dependency order. */
gt_status      gt_sup_add_child(gt_supervisor *s, const char *name, char *const argv[]);
/* One poll() iteration: reap SIGCHLD, check heartbeats, restart with backoff. */
gt_status      gt_sup_step(gt_supervisor *s, uint64_t now_ms);
void           gt_sup_destroy(gt_supervisor *s);

#endif
