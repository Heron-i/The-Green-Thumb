#ifndef SAMPLER_H
#define SAMPLER_H

#include <stdint.h>
#include "ipc.h"

/* Callbacks so the schedule can be tested without I2C hardware. */
typedef gt_status (*gt_read_fn)(void *ctx, uint32_t *raw_out);
typedef gt_status (*gt_emit_fn)(void *ctx, gt_sensor_id id, uint32_t raw);

typedef struct {
    gt_sensor_id id;
    uint32_t     period_ms;   /* 1000 light, 60000 soil */
    gt_read_fn   read;
    void        *read_ctx;
} gt_sampler_cfg;

typedef struct gt_sampler gt_sampler;

gt_sampler *gt_sampler_create(const gt_sampler_cfg *cfg, gt_emit_fn emit, void *emit_ctx);
/* One tick against a monotonic clock in ms; reads and emits if due. */
gt_status   gt_sampler_tick(gt_sampler *s, uint64_t now_ms);
void        gt_sampler_destroy(gt_sampler *s);

#endif
