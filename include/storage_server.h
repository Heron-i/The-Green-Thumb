#ifndef STORAGE_SERVER_H
#define STORAGE_SERVER_H

#include <stdatomic.h>
#include <stdint.h>
#include "gt_log.h"
#include "ipc.h"
#include "storage.h"

/* The two loops of the storage process (DESIGN.md "storage process": an
 * rx thread and a flush thread). They live in the library rather than in
 * storage_main so they can be tested: gt_storage_serve over a real socket
 * (tests/test_storage_serve.c), and one flush-thread pass with
 * tests/fake_file.h (tests/test_flusher_tick.c). storage_main only wires
 * them to the lock, the signals and stderr.
 *
 * Neither prints. Each event goes to a log callback: a pointer to a
 * function the caller provides, called with the level, the event name
 * (as in gt_log_format) and an optional "key=value" fields string. */
typedef void (*gt_storage_log_fn)(void *log_ctx, gt_log_level level, const char *event,
                                  const char *fields);

/* The rx loop, run on the process's main thread. poll() waits on three
 * things at once: the listening socket, the hub's connection (one at a
 * time) and stop_fd.
 *   - A hub connects: it is sent the seq_resume reply (gt_storage_resume)
 *     at once and becomes the connection; log hub_connected. A newer
 *     connection replaces an older one (log hub_duplicate_client), as for
 *     the hub's own sensor clients.
 *   - A frame arrives: gt_storage_rx decides; its events are logged, and
 *     a CLOSE closes the connection (log hub_disconnected).
 *   - stop_fd becomes readable (storage_main writes one byte to it on
 *     SIGTERM): frames the hub has already sent are still read and
 *     accepted, so nothing in the socket is lost, then it returns GT_OK.
 * The connection is closed on return; listen_fd and stop_fd are not.
 *   GT_OK on stop; GT_ERR_IO if poll fails; GT_ERR_INVAL for NULL storage
 *   or log, or a negative fd. */
gt_status gt_storage_serve(gt_storage *storage, int listen_fd, int stop_fd,
                           gt_storage_log_fn log, void *log_ctx);

/* The flush thread's state. */
typedef struct {
    gt_storage        *storage;
    unsigned           retention_days;   /* 0: GT_RETENTION_DAYS */
    gt_storage_log_fn  log;
    void              *log_ctx;
    int                logged_unsynced;  /* 1 while retention_skipped has been logged */
} gt_flusher;

void gt_flusher_init(gt_flusher *flusher, gt_storage *storage, unsigned retention_days,
                     gt_storage_log_fn log, void *log_ctx);

/* One pass of the flush thread: gt_storage_rollover, then gt_storage_flush
 * if gt_storage_flush_due says so, logging every event. retention_skipped
 * is logged only when the clock turns unsynced, not on every pass (an
 * unsynced Pi would otherwise log it once a second); it is logged again
 * after the clock has been synced in between. now_us is the wall clock;
 * now_ms the same monotonic clock appends use. NULL flusher is a no-op. */
void gt_flusher_tick(gt_flusher *flusher, uint64_t now_us, uint64_t now_ms, int clock_synced);

/* The flush thread's body: gt_flusher_tick once a second, with the real
 * clocks and clock_synced_fn(), until *stop is set. `atomic_int` is an int
 * that two threads can safely read and write at the same time (from
 * <stdatomic.h>); storage_main sets it, this loop reads it. A pass that
 * is due runs within about a second, which keeps a commit inside the
 * 62 s target. The final flush is not done here: storage_main calls
 * gt_storage_shutdown after joining this thread. */
void gt_flusher_run(gt_flusher *flusher, const atomic_int *stop, int (*clock_synced_fn)(void));

#endif
