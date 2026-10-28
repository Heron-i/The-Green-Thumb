#ifndef HUB_SERVER_H
#define HUB_SERVER_H

#include "gt_log.h"
#include "hub.h"
#include "hub_sender.h"
#include "ipc.h"

/* The hub process's rx loop (DESIGN.md "hub process": main thread,
 * poll() loop; Sensor-Pipeline.md, Hub steps). It lives in the library,
 * not in hub_main, so it can be tested over real sockets
 * (tests/test_hub_serve.c), as gt_storage_serve is. hub_main only wires
 * it to the lock, the signals and stderr.
 *
 * Each pass of one poll() loop:
 *   - every readable sensor connection: recv one 16 B message into a
 *     17 B buffer, gt_ipc_frame_decode it (a bad frame is ipc_bad_message
 *     and that connection is closed), gt_hub_accept it, and push the
 *     record to the sender. A sensor's first message on a new connection
 *     closes any older connection for the same sensor
 *     (sensor_duplicate_client: keep the newest).
 *   - accept one new sensor connection, up to GT_HUB_MAX_SENSOR_CONNS
 *   - gt_hub_check_timeouts, then gt_hub_sender_pump
 * poll() waits at most GT_HUB_POLL_MS, so the sender is pumped and the
 * timeouts are checked even when no sensor sends anything.
 *
 * Server side of the socket rule (PF-16): this file calls socket(),
 * accept() and recv() itself and is tested over real sockets; the sender
 * it pumps goes through the client seam. It reads the real clocks:
 * CLOCK_REALTIME for record stamps, CLOCK_MONOTONIC for timeouts and
 * backoff. Nothing is printed: every event goes to the log callback. */

/* Sensor connections served at once: light and soil, plus room for a
 * restarted sensor whose old connection hasn't been closed yet. */
#define GT_HUB_MAX_SENSOR_CONNS 8
#define GT_HUB_POLL_MS          100

/* The log callback: a pointer to a function the caller provides, called
 * with the level, the event name (as in gt_log_format) and a "key=value"
 * fields string, or NULL for none. Same shape as gt_storage_log_fn. */
typedef void (*gt_hub_log_fn)(void *log_ctx, gt_log_level level, const char *event,
                              const char *fields);

/* What the loop works with, in one struct so gt_hub_serve takes three
 * arguments rather than six. The loop uses them but owns none of them. */
typedef struct {
    gt_hub        *hub;
    gt_hub_sender *sender;
    gt_hub_log_fn  log;
    void          *log_ctx;
} gt_hub_serve_ctx;

/* Run the loop on the calling thread until stop_fd becomes readable
 * (hub_main writes one byte to it on SIGTERM). Then every message already
 * waiting on a sensor connection is still read, accepted and queued, the
 * sender is pumped once more, and the loop returns. Sensor connections are
 * closed on return; listen_fd and stop_fd are not.
 *   GT_OK on stop; GT_ERR_IO if poll() fails; GT_ERR_INVAL for a NULL ctx,
 *   hub, sender or log, or a negative fd. */
gt_status gt_hub_serve(const gt_hub_serve_ctx *ctx, int listen_fd, int stop_fd);

#endif
