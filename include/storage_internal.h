#ifndef STORAGE_INTERNAL_H
#define STORAGE_INTERNAL_H

/* Storage's private parts, shared by the files that make up the storage
 * object: storage.c (append, waits, stats), storage_open.c (open, close),
 * storage_flush.c (flush, rollover, shutdown) and storage_segment.c
 * (segment names). Only those files include this header; everything else
 * sees gt_storage only as the opaque type in storage.h, so it can't
 * reach into the struct. */
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "storage.h"
#include "storage_pending.h"

/* The clock gt_storage_wait_due measures its deadline on. On Linux the
 * condition variable is told to use CLOCK_MONOTONIC, so an NTP step
 * can't stretch or cut a wait. macOS has no pthread_condattr_setclock,
 * so there it stays on the default wall clock. */
#ifdef __linux__
#define WAIT_CLOCK CLOCK_MONOTONIC
#else
#define WAIT_CLOCK CLOCK_REALTIME
#endif

/* One open storage. `lock` is a pthread mutex: a lock that only one
 * thread can hold at a time. The rx thread (append) and the flush thread
 * both change this struct, so each takes the lock first
 * (pthread_mutex_lock) and gives it back after (pthread_mutex_unlock);
 * a thread that asks while another holds it waits. The lock is never held
 * during file I/O, so a slow fsync never stops append (DESIGN.md "The
 * lock is never held during I/O"). */
struct gt_storage {
    const gt_file_ops *ops;
    void              *ctx;
    int                handle;              /* the active segment; -1 if none */
    char               active_name[GT_FILE_NAME_MAX];
    uint64_t           committed_offset;    /* bytes of the active segment on disk */

    gt_pending        *pending;
    uint8_t           *flush_buf;           /* capacity * GT_REC_SIZE, used by flush */
    size_t             capacity;            /* records the pending buffer holds */

    pthread_mutex_t    lock;
    int                lock_ready;          /* 1 once pthread_mutex_init succeeded */
    /* due_cond is a condition variable: a place a thread can sleep, with
     * the lock let go, until another thread signals it. The flush thread
     * sleeps on it in gt_storage_wait_due; append signals it when a flush
     * becomes due (PF-11). due_signal says why it woke and is guarded by
     * `lock`, since a condition variable can also wake for no reason. */
    pthread_cond_t     due_cond;
    int                cond_ready;          /* 1 once pthread_cond_init succeeded */
    int                due_signal;          /* 1 from "flush became due" (or a wake) until a wait uses it */

    uint64_t           committed_records;
    int                has_last_accepted;
    uint32_t           last_accepted_seq;
    uint64_t           last_accepted_us;    /* that record's timestamp: a GAP's start (PF-07) */
    int                has_last_committed;
    uint32_t           last_committed_seq;
    int                degraded;            /* 1 from a failed flush until one succeeds */
    unsigned           backlog_percent;     /* 0, 50, 75 or 90 */
    uint64_t           refused;             /* readings refused because the buffer was full */
    int                full_episode;        /* 1 from a full refusal until a record is accepted */
    uint64_t           segments_deleted;    /* by retention, since open */

    gt_clock_us_fn     clock_us;            /* NULL: commits are not timed (PF-08) */
    uint64_t           commits;             /* successful flushes since open */
    size_t             last_commit_records;
    uint64_t           last_commit_us;      /* write (and any cut) + fsync */
    uint64_t           last_fsync_us;       /* the fsync alone */
};

/* The highest backlog threshold `pending` records have reached: 90, 75,
 * 50 or 0 (storage.c). Append and flush both use it. */
unsigned  gt_storage_backlog_level(size_t pending, size_t capacity);

/* Create segment `name` durably, header + fsync + fsync_dir
 * (storage_open.c). Open uses it for a new active segment and rollover
 * for the next day's. */
gt_status gt_storage_create_segment(const gt_file_ops *ops, void *ctx, const char *name,
                                    uint64_t now_us, int *handle_out);

#endif
