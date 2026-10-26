/* Linux implementation of the gt_file_ops seam (see storage_io.h).
 *
 * Purpose: this file is the only place storage makes file system calls.
 * It turns each op into one POSIX call (or a loop of them) on a file
 * inside the log directory, and what the kernel says into a gt_status. It
 * knows nothing about segments, batches or recovery: that policy lives in
 * storage.c and storage_recover.c, which reach this file only through the
 * ops table, so tests can swap in tests/fake_file.h.
 *
 * Linux model: the directory is opened once (O_DIRECTORY) and every name
 * is resolved relative to that open directory with the *at() calls
 * (openat, renameat, unlinkat), so the ops can only ever touch files in
 * it. A handle is the file's real descriptor. fsync on the directory's
 * descriptor is what makes a create, rename or delete durable on ext4.
 * Every descriptor is opened with O_CLOEXEC, so it closes automatically if
 * this process ever exec()s another program and can't leak into it.
 * How errno maps to a gt_status:
 *   EINTR (a signal arrived mid-call)  -> the call is simply tried again
 *   anything else                       -> GT_ERR_IO
 * A read that ends early (end of file) is GT_ERR_IO too: the seam promises
 * whole reads. */
#include "storage_io.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>    /* renameat: POSIX puts it here, next to rename */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The ctx: the log directory, open for as long as storage is. */
typedef struct {
    int dir_fd;
} linux_dir;

/* 1 if `name` is a plain file name inside the directory: not NULL, not
 * empty, short enough for a row, no '/', and not "." or "..". Anything
 * else could reach outside the directory. */
static int name_ok(const char *name)
{
    return name != NULL && name[0] != '\0' && strlen(name) < GT_FILE_NAME_MAX
           && strchr(name, '/') == NULL && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

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

static gt_status file_open(void *ctx, const char *name, unsigned flags, int *handle_out)
{
    linux_dir *dir = ctx;
    if (!name_ok(name) || handle_out == NULL) {
        return GT_ERR_INVAL;
    }
    int open_flags = O_RDWR | O_CLOEXEC | ((flags & GT_FILE_CREATE) ? O_CREAT : 0);
    int fd;
    /* Repeat only while a signal interrupted the call. */
    do {
        fd = openat(dir->dir_fd, name, open_flags, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return GT_ERR_IO;
    }
    *handle_out = fd;
    return GT_OK;
}

/* fstat fills a `struct stat` with facts about an open file; st_size is
 * its length in bytes. */
static gt_status file_size(void *ctx, int handle, uint64_t *size_out)
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
static gt_status file_pread(void *ctx, int handle, uint64_t offset, uint8_t *buf, size_t len)
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
static gt_status file_pwrite(void *ctx, int handle, uint64_t offset, const uint8_t *buf,
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

static gt_status file_fsync(void *ctx, int handle)
{
    (void)ctx;
    int result;
    /* Repeat only while a signal interrupted the call. */
    do {
        result = fsync(handle);
    } while (result != 0 && errno == EINTR);
    return result == 0 ? GT_OK : GT_ERR_IO;
}

static gt_status file_ftruncate(void *ctx, int handle, uint64_t len)
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
static void file_close(void *ctx, int handle)
{
    (void)ctx;
    close(handle);
}

static gt_status file_fsync_dir(void *ctx)
{
    linux_dir *dir = ctx;
    return file_fsync(ctx, dir->dir_fd);
}

static gt_status file_rename(void *ctx, const char *old_name, const char *new_name)
{
    linux_dir *dir = ctx;
    if (!name_ok(old_name) || !name_ok(new_name)) {
        return GT_ERR_INVAL;
    }
    return renameat(dir->dir_fd, old_name, dir->dir_fd, new_name) == 0 ? GT_OK : GT_ERR_IO;
}

static gt_status file_unlink(void *ctx, const char *name)
{
    linux_dir *dir = ctx;
    if (!name_ok(name)) {
        return GT_ERR_INVAL;
    }
    return unlinkat(dir->dir_fd, name, 0) == 0 ? GT_OK : GT_ERR_IO;
}

/* readdir works on a `DIR *` stream, which fdopendir makes from a
 * descriptor and closedir later closes. It is given a copy (dup) of the
 * directory's descriptor, so closedir can't close storage's own. A dup
 * shares its read position with the original, so the stream is rewound
 * first; otherwise a second listing would start where the first ended.
 * readdir returns NULL both at the end and on an error; errno is set to 0
 * first so the two can be told apart. */
static gt_status file_list(void *ctx, char names_out[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX],
                           size_t *count_out)
{
    linux_dir *dir = ctx;
    if (names_out == NULL || count_out == NULL) {
        return GT_ERR_INVAL;
    }
    int copy_fd = dup(dir->dir_fd);
    if (copy_fd < 0) {
        return GT_ERR_IO;
    }
    DIR *listing = fdopendir(copy_fd);
    if (listing == NULL) {
        close(copy_fd);
        return GT_ERR_IO;
    }
    rewinddir(listing);

    gt_status status = GT_OK;
    size_t count = 0;
    struct dirent *entry;
    /* invariant: names_out[0 .. count - 1] are the usable names read so far */
    while (status == GT_OK) {
        errno = 0;
        entry = readdir(listing);
        if (entry == NULL) {
            if (errno != 0) {
                status = GT_ERR_IO;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0
            || strlen(entry->d_name) >= GT_FILE_NAME_MAX) {
            continue;   /* storage never makes names that long */
        }
        if (count == GT_FILE_LIST_MAX) {
            status = GT_ERR_FULL;
            break;
        }
        strcpy(names_out[count], entry->d_name);
        count++;
    }
    closedir(listing);
    *count_out = count;
    return status;
}

static void file_destroy(void *ctx)
{
    linux_dir *dir = ctx;
    close(dir->dir_fd);
    free(dir);
}

gt_status gt_file_ops_linux(const char *dir, const gt_file_ops **ops_out, void **ctx_out)
{
    static const gt_file_ops ops = {
        .open      = file_open,
        .size      = file_size,
        .pread     = file_pread,
        .pwrite    = file_pwrite,
        .fsync     = file_fsync,
        .ftruncate = file_ftruncate,
        .close     = file_close,
        .fsync_dir = file_fsync_dir,
        .rename    = file_rename,
        .unlink    = file_unlink,
        .list      = file_list,
        .destroy   = file_destroy,
    };
    if (dir == NULL || dir[0] == '\0' || ops_out == NULL || ctx_out == NULL) {
        return GT_ERR_INVAL;
    }
    int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        return GT_ERR_IO;
    }
    linux_dir *ctx = malloc(sizeof *ctx);
    if (ctx == NULL) {
        close(dir_fd);
        return GT_ERR_NOMEM;
    }
    ctx->dir_fd = dir_fd;
    *ops_out = &ops;
    *ctx_out = ctx;
    return GT_OK;
}
