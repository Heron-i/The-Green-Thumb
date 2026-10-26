/* Storage: the one component that writes the log (see storage.h).
 *
 * Purpose: everything Mechanism D needs above the recovery scan and the
 * pending buffer:
 *   - segment naming: which file a record for a given UTC day goes in,
 *     and which files in the directory are segments at all (DESIGN.md
 *     "How records are organized");
 *   - the storage object: open (recovery, picking the active segment,
 *     creating it durably), append (check, then queue for the next
 *     flush), flush (write + fsync, the durability point), shutdown
 *     (final flush + clean-shutdown marker), stats and close.
 *
 * All file access goes through the gt_file_ops seam (storage_io.h), so
 * tests run on tests/fake_file.h. */
#include "storage.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "storage_pending.h"
#include "storage_recover.h"
#include "storage_seq.h"

#define US_PER_S     1000000u
#define S_PER_DAY    86400u
/* 9999-12-31 23:59:59 UTC, the last second whose year fits 4 digits. */
#define MAX_NAMED_S  253402300799u

#define SEGMENT_PREFIX     "log-"
#define SEGMENT_SUFFIX     ".bin"
#define SEGMENT_PREFIX_LEN 4
#define SEGMENT_DIGITS     8
#define SEGMENT_NAME_LEN   (SEGMENT_PREFIX_LEN + SEGMENT_DIGITS + 4)   /* 16 */

/* Turn a count of days since 1970-01-01 into a year, month (1-12) and day
 * (1-31) in the Gregorian calendar. This is Howard Hinnant's public-domain
 * "civil_from_days" algorithm, restricted to dates from 1970 on, so every
 * value is unsigned.
 *
 * Why not gmtime_r: on 32-bit Raspberry Pi OS, time_t is 32 bits, so
 * gmtime_r fails for every date after January 2038. This needs only
 * integer arithmetic and works the same on every platform.
 *
 * How it works: it shifts the calendar so each year starts on 1 March
 * (so the leap day is the last day of the year), then splits the days
 * into 400-year "eras" of exactly 146097 days, since the leap-year
 * pattern repeats every 400 years. Within an era it finds the year, then
 * the day of that year, then the month from a formula that fits the
 * month lengths starting from March (31, 30, 31, 30, 31, 31, 30, ...). */
static void civil_from_days(uint64_t days_since_1970, uint64_t *year_out,
                            unsigned *month_out, unsigned *day_out)
{
    uint64_t days = days_since_1970 + 719468;   /* days since 0000-03-01 */
    uint64_t era = days / 146097;
    uint64_t day_of_era = days - era * 146097;                       /* 0 .. 146096 */
    uint64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524
                            - day_of_era / 146096) / 365;             /* 0 .. 399 */
    uint64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4
                                         - year_of_era / 100);       /* 0 .. 365 */
    uint64_t month_from_march = (5 * day_of_year + 2) / 153;         /* 0 .. 11 */

    unsigned month = (unsigned)(month_from_march < 10 ? month_from_march + 3
                                                      : month_from_march - 9);
    *day_out = (unsigned)(day_of_year - (153 * month_from_march + 2) / 5 + 1);
    *month_out = month;
    *year_out = year_of_era + era * 400 + (month <= 2);   /* Jan and Feb belong to the next year */
}

/* Write `value` as exactly `width` decimal digits, with leading zeros,
 * into out[0 .. width-1]. The caller makes sure value fits. */
static void put_digits(char *out, uint64_t value, unsigned width)
{
    /* invariant: out[position .. width - 1] already hold value's lowest
     * digits, and value has had those digits divided off */
    for (unsigned position = width; position > 0; position--) {
        out[position - 1] = (char)('0' + value % 10);
        value /= 10;
    }
}

gt_status gt_segment_name(uint64_t now_us, char name_out[GT_FILE_NAME_MAX])
{
    uint64_t seconds = now_us / US_PER_S;
    if (name_out == NULL || seconds > MAX_NAMED_S) {
        return GT_ERR_INVAL;
    }

    uint64_t year;
    unsigned month, day;
    civil_from_days(seconds / S_PER_DAY, &year, &month, &day);

    char name[SEGMENT_NAME_LEN + 1];
    memcpy(name, SEGMENT_PREFIX, SEGMENT_PREFIX_LEN);
    put_digits(&name[4], year, 4);
    put_digits(&name[8], month, 2);
    put_digits(&name[10], day, 2);
    memcpy(&name[12], SEGMENT_SUFFIX, sizeof SEGMENT_SUFFIX);   /* includes the NUL */

    memcpy(name_out, name, sizeof name);
    return GT_OK;
}

/* Digits are checked as '0'..'9' directly rather than with isdigit(),
 * which depends on the locale and is undefined for negative char values. */
int gt_is_segment_name(const char *name)
{
    if (name == NULL || strlen(name) != SEGMENT_NAME_LEN
        || memcmp(name, SEGMENT_PREFIX, SEGMENT_PREFIX_LEN) != 0
        || strcmp(&name[SEGMENT_PREFIX_LEN + SEGMENT_DIGITS], SEGMENT_SUFFIX) != 0) {
        return 0;
    }
    for (size_t digit_index = 0; digit_index < SEGMENT_DIGITS; digit_index++) {
        char digit = name[SEGMENT_PREFIX_LEN + digit_index];
        if (digit < '0' || digit > '9') {
            return 0;
        }
    }
    return 1;
}

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

    uint64_t           committed_records;
    int                has_last_accepted;
    uint32_t           last_accepted_seq;
    int                has_last_committed;
    uint32_t           last_committed_seq;
    int                degraded;            /* 1 from a failed flush until one succeeds */
    unsigned           backlog_percent;     /* 0, 50, 75 or 90 */
    uint64_t           refused;             /* readings refused because the buffer was full */
    int                full_episode;        /* 1 from a full refusal until a record is accepted */
    uint64_t           segments_deleted;    /* by retention, since open */
};

/* Close the active segment, free everything and destroy ctx: the one
 * clean-up path for both close and a failed open. */
static void free_storage(gt_storage *storage)
{
    if (storage->handle >= 0) {
        storage->ops->close(storage->ctx, storage->handle);
    }
    if (storage->lock_ready) {
        pthread_mutex_destroy(&storage->lock);
    }
    gt_pending_destroy(storage->pending);
    free(storage->flush_buf);
    storage->ops->destroy(storage->ctx);
    free(storage);
}

/* The memory half of open, done before any file is touched, so running
 * out of memory never leaves a half-made segment. */
static gt_status alloc_storage(const gt_file_ops *ops, void *ctx, size_t pending_capacity,
                               gt_storage **storage_out)
{
    gt_storage *storage = calloc(1, sizeof *storage);   /* calloc zeroes the struct */
    if (storage == NULL) {
        return GT_ERR_NOMEM;
    }
    storage->ops = ops;
    storage->ctx = ctx;
    storage->handle = -1;
    storage->capacity = pending_capacity != 0 ? pending_capacity : GT_PENDING_MAX_RECORDS;
    storage->pending = gt_pending_create(storage->capacity);   /* checks capacity * 24 fits */
    if (storage->pending != NULL) {
        storage->flush_buf = malloc(storage->capacity * GT_REC_SIZE);
    }
    if (storage->flush_buf != NULL && pthread_mutex_init(&storage->lock, NULL) == 0) {
        storage->lock_ready = 1;
    }
    if (!storage->lock_ready) {
        free_storage(storage);   /* also destroys ctx: storage owns it now */
        return GT_ERR_NOMEM;
    }
    *storage_out = storage;
    return GT_OK;
}

/* List the directory and keep only segment names, sorted oldest first
 * (alphabetical order is date order, see storage.h). *marker_out is 1 if
 * the clean-shutdown marker is there too. */
static gt_status list_segments(const gt_file_ops *ops, void *ctx,
                               char segments[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX],
                               size_t *count_out, int *marker_out)
{
    char names[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX];
    size_t name_count = 0;
    if (ops->list(ctx, names, &name_count) != GT_OK) {
        return GT_ERR_IO;
    }

    size_t count = 0;
    *marker_out = 0;
    for (size_t name_index = 0; name_index < name_count; name_index++) {
        if (strcmp(names[name_index], GT_STORAGE_MARKER_NAME) == 0) {
            *marker_out = 1;
        } else if (gt_is_segment_name(names[name_index])) {
            memcpy(segments[count], names[name_index], GT_FILE_NAME_MAX);
            count++;
        }
    }

    /* Insertion sort: there are at most about 31 segments.
     * invariant: segments[0 .. sorted - 1] are in order */
    for (size_t sorted = 1; sorted < count; sorted++) {
        char moving[GT_FILE_NAME_MAX];
        memcpy(moving, segments[sorted], GT_FILE_NAME_MAX);
        size_t slot = sorted;
        /* invariant: segments[slot + 1 .. sorted] are all greater than moving */
        while (slot > 0 && strcmp(segments[slot - 1], moving) > 0) {
            memcpy(segments[slot], segments[slot - 1], GT_FILE_NAME_MAX);
            slot--;
        }
        memcpy(segments[slot], moving, GT_FILE_NAME_MAX);
    }
    *count_out = count;
    return GT_OK;
}

/* Steps 2 and 3 of open: recover the newest segment, then walk back until
 * a segment with records gives the newest seq on disk. Older segments
 * were complete when the day rolled over, so recovering them normally
 * changes nothing; it is just how their last seq is read. On return,
 * *newest_out is the index of the newest segment that still exists, or
 * -1 if every segment visited was renamed .corrupt (or there are none). */
static gt_status find_newest(gt_storage *storage,
                             char segments[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX],
                             size_t count, gt_storage_open_info *info, long *newest_out)
{
    long newest = -1;
    /* invariant: segments[visit .. count - 1] have been recovered, and
     * none of them that still exists holds a record */
    for (size_t visit = count; visit > 0; visit--) {
        const char *name = segments[visit - 1];
        gt_recover_report report;
        gt_status status = gt_storage_recover_ops(storage->ops, storage->ctx, name, &report);
        if (status != GT_OK) {
            return status;
        }
        if (visit == count) {
            info->recovered = 1;
            memcpy(info->recovered_name, name, GT_FILE_NAME_MAX);
            info->recovery = report;
        }
        if (report.outcome == GT_RECOVER_RENAMED) {
            continue;
        }
        if (newest < 0) {
            newest = (long)(visit - 1);
        }
        if (report.has_records) {
            info->has_last = 1;
            info->last_seq = report.last_seq;
            break;
        }
    }
    *newest_out = newest;
    return GT_OK;
}

/* Steps 4 and 5 of open: make `name` the active segment. An existing one
 * is opened and appended to at its end; a new one gets its header, then
 * fsync (the header is on the card), then fsync_dir (the name is too). */
static gt_status open_active(gt_storage *storage, const char *name, int create, uint64_t now_us)
{
    const gt_file_ops *ops = storage->ops;
    void *ctx = storage->ctx;

    if (ops->open(ctx, name, create ? GT_FILE_CREATE : 0, &storage->handle) != GT_OK) {
        storage->handle = -1;
        return GT_ERR_IO;
    }
    memcpy(storage->active_name, name, GT_FILE_NAME_MAX);

    if (!create) {
        return ops->size(ctx, storage->handle, &storage->committed_offset);
    }
    const gt_seg_header header = { .created_at_us = now_us };
    uint8_t header_bytes[GT_SEG_HEADER_SZ];
    if (gt_seg_header_encode(&header, header_bytes) != GT_OK
        || ops->pwrite(ctx, storage->handle, 0, header_bytes, sizeof header_bytes) != GT_OK
        || ops->fsync(ctx, storage->handle) != GT_OK
        || ops->fsync_dir(ctx) != GT_OK) {
        return GT_ERR_IO;
    }
    storage->committed_offset = GT_SEG_HEADER_SZ;
    return GT_OK;
}

gt_status gt_storage_open_ops(const gt_file_ops *ops, void *ctx, uint64_t now_us,
                              size_t pending_capacity, gt_storage **storage_out,
                              gt_storage_open_info *info_out)
{
    if (ops == NULL) {
        return GT_ERR_INVAL;   /* no destroy to call: the caller keeps ctx */
    }
    char today[GT_FILE_NAME_MAX];
    if (storage_out == NULL || gt_segment_name(now_us, today) != GT_OK) {
        ops->destroy(ctx);
        return GT_ERR_INVAL;
    }

    gt_storage *storage = NULL;
    gt_status status = alloc_storage(ops, ctx, pending_capacity, &storage);
    if (status != GT_OK) {
        return status;   /* alloc_storage already destroyed ctx */
    }

    gt_storage_open_info info;
    memset(&info, 0, sizeof info);
    char segments[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX];
    size_t segment_count = 0;
    int marker_present = 0;
    long newest = -1;

    status = list_segments(ops, ctx, segments, &segment_count, &marker_present);
    if (status == GT_OK) {
        /* Step 0: read the marker, then delete it durably at once. */
        info.unclean_shutdown = !marker_present && segment_count > 0;
        if (marker_present
            && (ops->unlink(ctx, GT_STORAGE_MARKER_NAME) != GT_OK
                || ops->fsync_dir(ctx) != GT_OK)) {
            status = GT_ERR_IO;
        }
    }
    if (status == GT_OK) {
        status = find_newest(storage, segments, segment_count, &info, &newest);
    }
    if (status == GT_OK) {
        /* Reuse the newest segment if it is today's or later (the clock may
         * be behind); otherwise start today's. */
        int reuse = newest >= 0 && strcmp(segments[newest], today) >= 0;
        info.created = !reuse;
        status = open_active(storage, reuse ? segments[newest] : today, !reuse, now_us);
    }
    if (status != GT_OK) {
        free_storage(storage);
        return status;
    }

    memcpy(info.active_name, storage->active_name, GT_FILE_NAME_MAX);
    storage->has_last_accepted = storage->has_last_committed = info.has_last;
    storage->last_accepted_seq = storage->last_committed_seq = info.last_seq;
    if (info_out != NULL) {
        *info_out = info;
    }
    *storage_out = storage;
    return GT_OK;
}

/* The highest backlog threshold `pending` records have reached: 90, 75,
 * 50 or 0 (DESIGN.md "storage_backlog at 50/75/90%"). Integer-only:
 * pending * 100 >= capacity * threshold. Widened to uint64_t so the
 * multiplication can't overflow even where size_t is 32 bits. */
static unsigned backlog_level(size_t pending, size_t capacity)
{
    static const unsigned thresholds[] = { 90, 75, 50 };
    uint64_t scaled_pending = (uint64_t)pending * 100u;

    for (size_t threshold_index = 0; threshold_index < 3; threshold_index++) {
        if (scaled_pending >= (uint64_t)capacity * thresholds[threshold_index]) {
            return thresholds[threshold_index];
        }
    }
    return 0;
}

/* A reading refused because the buffer is full: count it, and report the
 * first refusal of a full episode once. */
static gt_status refuse_full(gt_storage *storage, unsigned *events)
{
    storage->refused++;
    if (!storage->full_episode) {
        storage->full_episode = 1;
        *events |= GT_STORAGE_EV_QUEUE_FULL;
    }
    return GT_ERR_FULL;
}

/* The part of append that runs under the lock: judge the seq, then store
 * the record (and its GAP first, if seqs are missing). A GAP needs two
 * free slots, checked before either is added, so it is both or neither.
 * The seq check comes before any room check, so a duplicate arriving
 * while full is rejected as a duplicate, never counted as refused. */
static gt_status append_locked(gt_storage *storage, const uint8_t record[GT_REC_SIZE],
                               const gt_record *decoded, uint64_t now_ms, unsigned *events)
{
    gt_seq_verdict verdict = gt_seq_check(storage->last_accepted_seq,
                                          storage->has_last_accepted, decoded->seq);
    if (verdict == GT_SEQ_REJECT) {
        *events |= GT_STORAGE_EV_SEQ_REJECTED;
        return GT_ERR_CORRUPT;
    }

    size_t room = storage->capacity - gt_pending_count(storage->pending);
    if (room < (verdict == GT_SEQ_GAP ? 2u : 1u)) {
        return refuse_full(storage, events);
    }

    if (verdict == GT_SEQ_GAP) {
        gt_record gap;
        uint8_t gap_wire[GT_REC_SIZE];
        if (gt_seq_gap_record(storage->last_accepted_seq, decoded, &gap) != GT_OK
            || gt_record_encode(&gap, gap_wire) != GT_OK
            || gt_pending_add(storage->pending, gap_wire, now_ms) != GT_OK) {
            return refuse_full(storage, events);   /* can't happen after the room check */
        }
        *events |= GT_STORAGE_EV_RECOVERY_GAP;
    }
    if (gt_pending_add(storage->pending, record, now_ms) != GT_OK) {
        return refuse_full(storage, events);       /* can't happen after the room check */
    }

    storage->has_last_accepted = 1;
    storage->last_accepted_seq = decoded->seq;
    storage->full_episode = 0;

    unsigned level = backlog_level(gt_pending_count(storage->pending), storage->capacity);
    if (level > storage->backlog_percent) {
        *events |= GT_STORAGE_EV_BACKLOG;
    }
    storage->backlog_percent = level;
    return GT_OK;
}

/* Decoding needs no lock (it only reads `record`), so it runs first and a
 * bad record never takes the lock at all. */
gt_status gt_storage_append(gt_storage *storage, const uint8_t record[GT_REC_SIZE],
                            uint64_t now_ms, unsigned *events_out)
{
    unsigned events = 0;
    gt_status status;
    gt_record decoded;

    if (storage == NULL || record == NULL) {
        status = GT_ERR_INVAL;
    } else if (gt_record_decode(record, &decoded) != GT_OK) {
        status = GT_ERR_CORRUPT;
    } else {
        pthread_mutex_lock(&storage->lock);
        status = append_locked(storage, record, &decoded, now_ms, &events);
        pthread_mutex_unlock(&storage->lock);
    }
    if (events_out != NULL) {
        *events_out = events;
    }
    return status;
}

int gt_storage_flush_due(gt_storage *storage, uint64_t now_ms)
{
    if (storage == NULL) {
        return 0;
    }
    pthread_mutex_lock(&storage->lock);
    int due = gt_pending_due(storage->pending, now_ms);
    pthread_mutex_unlock(&storage->lock);
    return due;
}

/* Step 2 of flush: put byte_count bytes of the flush buffer on the card.
 * After a failed flush (degraded), the file may end in torn bytes past
 * the committed offset, or hold a full copy that was never fsynced, so
 * it is first cut back to the committed offset. Writing on top of torn
 * bytes is never allowed: if the cut fails, nothing is written. Returns
 * GT_OK, or GT_ERR_IO with WRITE_ERROR or FSYNC_ERROR added to *events. */
static gt_status write_batch(gt_storage *storage, size_t byte_count, unsigned *events)
{
    const gt_file_ops *ops = storage->ops;

    if (storage->degraded
        && ops->ftruncate(storage->ctx, storage->handle, storage->committed_offset) != GT_OK) {
        *events |= GT_STORAGE_EV_WRITE_ERROR;
        return GT_ERR_IO;
    }
    if (ops->pwrite(storage->ctx, storage->handle, storage->committed_offset,
                    storage->flush_buf, byte_count) != GT_OK) {
        *events |= GT_STORAGE_EV_WRITE_ERROR;
        return GT_ERR_IO;
    }
    if (ops->fsync(storage->ctx, storage->handle) != GT_OK) {
        *events |= GT_STORAGE_EV_FSYNC_ERROR;
        return GT_ERR_IO;
    }
    return GT_OK;
}

/* handle, committed_offset and degraded are read outside the lock: only
 * the one flushing thread ever changes them (see storage.h), and it
 * changes degraded under the lock so read_stats sees a consistent value.
 * Everything append touches is changed under the lock. */
gt_status gt_storage_flush(gt_storage *storage, unsigned *events_out)
{
    unsigned events = 0;
    if (events_out != NULL) {
        *events_out = 0;
    }
    if (storage == NULL) {
        return GT_ERR_INVAL;
    }

    /* 1. Copy the pending records out under the lock. */
    size_t record_count = 0;
    pthread_mutex_lock(&storage->lock);
    const uint8_t *pending_bytes = gt_pending_peek(storage->pending, &record_count);
    if (record_count > 0) {
        memcpy(storage->flush_buf, pending_bytes, record_count * GT_REC_SIZE);
    }
    pthread_mutex_unlock(&storage->lock);
    if (record_count == 0) {
        return GT_OK;
    }

    /* 2. Write and fsync with no lock held, so append never waits on the card. */
    size_t byte_count = record_count * GT_REC_SIZE;
    if (write_batch(storage, byte_count, &events) != GT_OK) {
        /* The records are still pending; mark degraded (reported once). */
        pthread_mutex_lock(&storage->lock);
        if (!storage->degraded) {
            storage->degraded = 1;
            events |= GT_STORAGE_EV_DEGRADED;
        }
        pthread_mutex_unlock(&storage->lock);
        if (events_out != NULL) {
            *events_out = events;
        }
        return GT_ERR_IO;
    }

    /* 3. They are committed: drop them and move the counters on. Every
     * pending record was checked by append, so the last one decodes. */
    gt_record last_record;
    gt_status decoded = gt_record_decode(&storage->flush_buf[byte_count - GT_REC_SIZE],
                                         &last_record);
    pthread_mutex_lock(&storage->lock);
    gt_pending_commit(storage->pending, record_count);
    /* A commit only removes records, so the level can only go down, and
     * going down is silent; it re-arms the thresholds for the next rise. */
    storage->backlog_percent = backlog_level(gt_pending_count(storage->pending),
                                             storage->capacity);
    storage->committed_offset += byte_count;
    storage->committed_records += record_count;
    if (decoded == GT_OK) {
        storage->has_last_committed = 1;
        storage->last_committed_seq = last_record.seq;
    }
    if (storage->degraded) {
        storage->degraded = 0;
        events |= GT_STORAGE_EV_RECOVERED;
    }
    pthread_mutex_unlock(&storage->lock);
    if (events_out != NULL) {
        *events_out = events;
    }
    return GT_OK;
}

/* The marker is an empty file, so only its directory entry has to be
 * made durable: fsync_dir, no fsync of the file itself. */
gt_status gt_storage_shutdown(gt_storage *storage, unsigned *events_out)
{
    if (events_out != NULL) {
        *events_out = 0;
    }
    if (storage == NULL) {
        return GT_ERR_INVAL;
    }

    gt_status status = gt_storage_flush(storage, events_out);
    if (status != GT_OK) {
        return status;   /* no marker: this shutdown lost records */
    }

    int marker_handle = -1;
    if (storage->ops->open(storage->ctx, GT_STORAGE_MARKER_NAME, GT_FILE_CREATE,
                           &marker_handle) != GT_OK) {
        status = GT_ERR_IO;
    } else {
        storage->ops->close(storage->ctx, marker_handle);
        if (storage->ops->fsync_dir(storage->ctx) != GT_OK) {
            status = GT_ERR_IO;
        }
    }
    if (status != GT_OK && events_out != NULL) {
        *events_out |= GT_STORAGE_EV_WRITE_ERROR;
    }
    return status;
}

/* stats_out is filled under the lock so its fields all come from the same
 * moment. The lock is changed by locking, so the `const` is cast away for
 * that one call; the storage itself was never defined const, so this is
 * allowed. */
gt_status gt_storage_read_stats(const gt_storage *storage, gt_storage_stats *stats_out)
{
    if (storage == NULL || stats_out == NULL) {
        return GT_ERR_INVAL;
    }
    pthread_mutex_t *lock = (pthread_mutex_t *)&storage->lock;
    pthread_mutex_lock(lock);
    stats_out->pending = gt_pending_count(storage->pending);
    stats_out->committed_records = storage->committed_records;
    stats_out->has_last_accepted = storage->has_last_accepted;
    stats_out->last_accepted_seq = storage->last_accepted_seq;
    stats_out->has_last_committed = storage->has_last_committed;
    stats_out->last_committed_seq = storage->last_committed_seq;
    stats_out->degraded = storage->degraded;
    stats_out->backlog_percent = storage->backlog_percent;
    stats_out->refused = storage->refused;
    stats_out->segments_deleted = storage->segments_deleted;
    pthread_mutex_unlock(lock);
    return GT_OK;
}

void gt_storage_close(gt_storage *storage)
{
    if (storage == NULL) {
        return;
    }
    free_storage(storage);
}
