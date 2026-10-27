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

/* pread may return fewer bytes than asked for, so it is called until all
 * `len` bytes are in. 0 bytes back means the end of the file came first. */
gt_status gt_file_pread_linux(void *ctx, int handle, uint64_t offset, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (buf == NULL) {
        return GT_ERR_INVAL;
    }
    size_t done = 0;
    /* invariant: buf[0 .. done - 1] holds the file's bytes from offset on */
    while (done < len) {
        off_t position;
        if (!to_off_t(offset + done, &position)) {
            return GT_ERR_IO;
        }
        ssize_t got = pread(handle, &buf[done], len - done, position);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return GT_ERR_IO;   /* an error, or the end of the file: a short read */
        }
        done += (size_t)got;
    }
    return GT_OK;
}

/* pwrite may write fewer bytes than asked for (a "short write"), so it is
 * called until all `len` bytes are out. A failure part-way leaves the
 * bytes already written in the file: the torn write storage truncates. */
gt_status gt_file_pwrite_linux(void *ctx, int handle, uint64_t offset, const uint8_t *buf,
                               size_t len)
{
    (void)ctx;
    if (buf == NULL) {
        return GT_ERR_INVAL;
    }
    size_t done = 0;
    /* invariant: buf[0 .. done - 1] is in the file from offset on */
    while (done < len) {
        off_t position;
        if (!to_off_t(offset + done, &position)) {
            return GT_ERR_IO;
        }
        ssize_t wrote = pwrite(handle, &buf[done], len - done, position);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            return GT_ERR_IO;   /* 0 would loop forever, so it counts as a failure */
        }
        done += (size_t)wrote;
    }
    return GT_OK;
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
