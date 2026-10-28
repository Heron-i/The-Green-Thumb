#ifndef LIGHT_LOOP_H
#define LIGHT_LOOP_H

#include <stdint.h>

#include "gt_log.h"
#include "ipc.h"
#include "light_sensor.h"
#include "sensor_health.h"

/* The light process's loop (DESIGN.md "light_sensor process": main thread,
 * read -> send; Sensor-Pipeline.md, Hub steps H10). It lives in the
 * library, not in light_sensor_main, so it can be tested against
 * tests/fake_i2c.h and tests/fake_sock.h with no clock (LLP). The main only
 * sleeps until the due time each tick returns, and prints log lines.
 *
 * One tick:
 *   1. If a reading is due and gt_health_should_read says yes, read it.
 *      While offline, a good probe read first re-runs the chip's init
 *      (gt_light_reinit, sensor_reinit) and then reads again, because a
 *      replugged chip has lost its mode and its first value is stale.
 *   2. Feed the result to the health tracker and log what it says:
 *      i2c_read_error on every failed read, sensor_offline /
 *      sensor_online on a transition.
 *   3. A good value is sent as one DATA message. 0xFFFF is still sent (it
 *      is real data) and also logs light_saturated.
 *   4. With nothing sent, pump the client so its ring still drains. Log
 *      the client's events: ipc_disconnect, ipc_reconnect, ipc_overflow.
 *   5. Move the due time on by one period. If `now` is already past it,
 *      skip the missed slots instead of reading in a burst, keeping the
 *      schedule anchored, and log sample_overrun skipped=N.
 *
 * The counter: each message carries the sensor-side counter, which goes up
 * once per reading handed to the client, sent or refused (a refused
 * reading is counted into the client's GAP, which the hub expects to cover
 * those counters). A failed read makes no message, so it uses no counter
 * and the hub sees no sensor_seq_gap. */

#define GT_LIGHT_PERIOD_MS 1000   /* one reading per second (DESIGN.md "At what rate") */

/* The log callback: a pointer to a function the caller provides, called
 * with the level, the event name and a "key=value" fields string, or NULL
 * for none. Same shape as gt_hub_log_fn. */
typedef void (*gt_light_log_fn)(void *log_ctx, gt_log_level level, const char *event,
                                const char *fields);

/* What the loop works with. The loop uses them but owns none of them: the
 * caller opens them before gt_light_loop_init and closes them after. */
typedef struct {
    gt_light_sensor *sensor;
    gt_ipc_client   *client;
    gt_light_log_fn  log;
    void            *log_ctx;
} gt_light_loop_ctx;

/* The loop's state. A public struct with no malloc, like gt_health: the
 * caller declares one (on the stack, say) and passes its address. Read the
 * fields in tests; change them only through these functions. */
typedef struct {
    gt_light_loop_ctx ctx;
    gt_health         health;        /* online/offline tracking */
    uint32_t          counter;       /* the counter the next message will carry */
    uint64_t          next_due_ms;   /* monotonic ms when the next reading is due */
} gt_light_loop;

/* Set up `loop` for `ctx` (copied in) with the sensor online, the counter
 * at 0 and the first reading due at now_ms. GT_ERR_INVAL for a NULL loop,
 * ctx, sensor, client or log, and *loop is untouched. */
gt_status gt_light_loop_init(gt_light_loop *loop, const gt_light_loop_ctx *ctx, uint64_t now_ms);

/* Run one tick at monotonic time now_ms (see the top of this file). Sets
 * *next_due_out to when the next reading is due, the time the caller
 * sleeps until (pass NULL to ignore it). Never blocks: a failed read and a
 * missing hub are logged, not returned.
 *   GT_OK, or GT_ERR_INVAL for a NULL loop (*next_due_out untouched). */
gt_status gt_light_loop_tick(gt_light_loop *loop, uint64_t now_ms, uint64_t *next_due_out);

#endif
