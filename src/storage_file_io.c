/* Linux file layer, part 1: operations on an open file (see storage_io.h
 * and storage_file_internal.h).
 *
 * Purpose: size, read, write, fsync, truncate and close one already-open
 * segment. Each is one POSIX call on the file's descriptor (the handle),
 * or a loop of them, turned into a gt_status. These never touch names or
 * the directory; that half, and the ops table, is storage_file.c.
 *
 * How errno maps to a gt_status:
 *   EINTR (a signal arrived mid-call)  -> the call is simply tried again
 *   anything else                       -> GT_ERR_IO
 * A read that ends early (end of file) is GT_ERR_IO too: the seam promises
 * whole reads. */
#include "storage_file_internal.h"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* Convert a byte offset to off_t, the type the kernel calls use. off_t is
 * signed and, on a 32-bit build, only 32 bits, so the value is checked to
 * survive the conversion unchanged. Returns 1 if it fits. */
static int to_off_t(uint64_t value, off_t *out)
{
    off_t converted = (off_t)value;
    if (converted < 0 || (uint64_t)converted != value) {
        return 0;
    }
    *out = converted;
    return 1;
}

/* fstat fills a `struct stat` with facts about an open file; st_size is
 * its length in bytes. */
gt_status gt_file_size_linux(void *ctx, int handle, uint64_t *size_out)
{
    (void)ctx;
    struct stat info;
    if (size_out == NULL) {
        return GT_ERR_INVAL;
    }
    if (fstat(handle, &info) != 0 || info.st_size < 0) {
        return GT_ERR_IO;
    }
    *size_out = (uint64_t)info.st_size;
    return GT_OK;
}

/* One whole read or write: which file, where, how much. Only one of the
 * two buffers is set; each step function uses the one it needs, so the
 * write side never has to cast away `const`. */
struct transfer {
    int handle;
    uint64_t offset;
    size_t len;
    uint8_t *read_buf;          /* set for reads  */
    const uint8_t *write_buf;   /* set for writes */
};

/* A function pointer: a variable that holds which function to call (like
 * gt_read_fn in sampler.h). A step moves bytes [done, len) at `position`
 * with one syscall and returns what that syscall returned. */
typedef ssize_t (*transfer_step_fn)(const struct transfer *job, size_t done, off_t position);

static ssize_t read_step(const struct transfer *job, size_t done, off_t position)
{
    return pread(job->handle, &job->read_buf[done], job->len - done, position);
}

static ssize_t write_step(const struct transfer *job, size_t done, off_t position)
{
    return pwrite(job->handle, &job->write_buf[done], job->len - done, position);
}

/* pread and pwrite may move fewer bytes than asked for, so the step is
 * called until all `len` bytes are moved. */
static gt_status transfer_all(const struct transfer *job, transfer_step_fn step)
{
    size_t done = 0;
    /* invariant: bytes [0, done) of the buffer have been moved from offset on */
    while (done < job->len) {
        off_t position;
        if (!to_off_t(job->offset + done, &position)) {
            return GT_ERR_IO;
        }
        ssize_t moved = step(job, done, position);
        if (moved < 0 && errno == EINTR) {
            continue;
        }
        if (moved <= 0) {
            /* an error; or 0 bytes: end of file on a read (a short read),
             * and on a write it would loop forever */
            return GT_ERR_IO;
        }
        done += (size_t)moved;
    }
    return GT_OK;
}

/* Reads exactly `len` bytes; reaching the end of the file first is a
 * short read and fails. */
gt_status gt_file_pread_linux(void *ctx, int handle, uint64_t offset, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (buf == NULL) {
        return GT_ERR_INVAL;
    }
    struct transfer job = { .handle = handle, .offset = offset, .len = len, .read_buf = buf };
    return transfer_all(&job, read_step);
}

/* Writes exactly `len` bytes. A failure part-way leaves the bytes already
 * written in the file: the torn write storage truncates. */
gt_status gt_file_pwrite_linux(void *ctx, int handle, uint64_t offset, const uint8_t *buf,
                               size_t len)
{
    (void)ctx;
    if (buf == NULL) {
        return GT_ERR_INVAL;
    }
    struct transfer job = { .handle = handle, .offset = offset, .len = len, .write_buf = buf };
    return transfer_all(&job, write_step);
}

/* storage_file.c's fsync_dir uses this too, on the directory's own
 * descriptor. */
gt_status gt_file_fsync_linux(void *ctx, int handle)
{
    (void)ctx;
    int result;
    /* Repeat only while a signal interrupted the call. */
    do {
        result = fsync(handle);
    } while (result != 0 && errno == EINTR);
    return result == 0 ? GT_OK : GT_ERR_IO;
}

gt_status gt_file_ftruncate_linux(void *ctx, int handle, uint64_t len)
{
    (void)ctx;
    off_t length;
    if (!to_off_t(len, &length)) {
        return GT_ERR_IO;
    }
    int result;
    /* Repeat only while a signal interrupted the call. */
    do {
        result = ftruncate(handle, length);
    } while (result != 0 && errno == EINTR);
    return result == 0 ? GT_OK : GT_ERR_IO;
}

/* Not retried on EINTR: on Linux the descriptor is already closed by
 * then, and closing it again could close a different file. */
void gt_file_close_linux(void *ctx, int handle)
{
    (void)ctx;
    close(handle);
}
