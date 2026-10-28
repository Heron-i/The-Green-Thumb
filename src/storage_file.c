/* Linux implementation of the gt_file_ops seam (see storage_io.h), part
 * 2: the directory, and the ops table.
 *
 * Purpose: storage_file.c and storage_file_io.c are the only places
 * storage makes file system calls. They turn each op into one POSIX call
 * (or a loop of them) on a file inside the log directory, and what the
 * kernel says into a gt_status. They know nothing about segments, batches
 * or recovery: that policy lives in storage*.c and storage_recover.c,
 * which reach them only through the ops table, so tests can swap in
 * tests/fake_file.h. This file holds the ops that use names (open,
 * rename, unlink, list) and the directory itself; the ops on an open
 * file's handle are in storage_file_io.c.
 *
 * Linux model: the directory is opened once (O_DIRECTORY) and every name
 * is resolved relative to that open directory with the *at() calls
 * (openat, renameat, unlinkat), so the ops can only ever touch files in
 * it. A handle is the file's real descriptor. fsync on the directory's
 * descriptor is what makes a create, rename or delete durable on ext4.
 * Every descriptor is opened with O_CLOEXEC, so it closes automatically if
 * this process ever exec()s another program and can't leak into it.
 * EINTR (a signal arrived mid-call) is retried; any other error is
 * GT_ERR_IO. */
#include "storage_file_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>    /* renameat: POSIX puts it here, next to rename */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The ctx: the log directory, open for as long as storage is. */
typedef struct {
    int dir_fd;
} linux_dir;

/* 1 if `name` is "." or "..": the directory itself or its parent. */
static int is_dot_name(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/* 1 if `name` is a plain file name inside the directory: not NULL, not
 * empty, short enough for a row, no '/', and not "." or "..". Anything
 * else could reach outside the directory. */
static int name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= GT_FILE_NAME_MAX) {
        return 0;
    }
    return strchr(name, '/') == NULL && !is_dot_name(name);
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

static gt_status file_fsync_dir(void *ctx)
{
    linux_dir *dir = ctx;
    return gt_file_fsync_linux(ctx, dir->dir_fd);
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
        size_t name_len = strlen(entry->d_name);
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0
            || name_len >= GT_FILE_NAME_MAX) {
            continue;   /* storage never makes names that long */
        }
        if (count == GT_FILE_LIST_MAX) {
            status = GT_ERR_FULL;
            break;
        }
        /* memcpy of the measured length (+1 for the NUL) rather than
         * strcpy: d_name can be 255 bytes, and gcc can't always prove the
         * check above keeps it inside the 32-byte row. */
        memcpy(names_out[count], entry->d_name, name_len + 1);
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
        .size      = gt_file_size_linux,
        .pread     = gt_file_pread_linux,
        .pwrite    = gt_file_pwrite_linux,
        .fsync     = gt_file_fsync_linux,
        .ftruncate = gt_file_ftruncate_linux,
        .close     = gt_file_close_linux,
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
