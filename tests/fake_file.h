#ifndef FAKE_FILE_H
#define FAKE_FILE_H

/* Fake gt_file_ops for storage tests: an in-memory directory, no disk.
 *
 * A test declares a fake_fs, passes fake_fs_ops(&fs) and &fs to storage,
 * optionally puts segment files in it first (fake_fs_put), scripts which
 * call should fail, then inspects the files and counters. The fake is the
 * ctx and is owned by the test, so destroy() only counts and never frees.
 * fake_fs_ops() zeroes the fake, so call it before anything else.
 *
 * A fake_fs is about 70 KB, so declare it `static` inside the test
 * function rather than as a plain local, to keep it off the stack.
 *
 * Power-cut model: every file has two copies of its contents. The
 * "current" copy is what reads see (like the kernel's page cache); the
 * "synced" copy is what survives a power cut (the card). fsync copies
 * current to synced. The directory works the same way: a create, rename
 * or unlink is current at once but only synced by fsync_dir.
 * fake_fs_power_cut() throws away everything not synced. This is the
 * strictest reading of POSIX: nothing is durable without its fsync, which
 * is exactly what DESIGN.md's durability rules assume.
 *
 * Header-only (static inline) for the same reason as fake_i2c.h: the
 * Makefile links each test .c with src/ only, and inline keeps unused
 * helpers from tripping -Werror. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "storage_io.h"

#define FAKE_FS_MAX_FILES  8
#define FAKE_FILE_MAX_BYTES 4096   /* header + 170 records: enough for 2 batches */

typedef struct {
    /* Current view: what the running program sees. */
    int      used;                       /* the name exists in the directory */
    char     name[GT_FILE_NAME_MAX];
    uint8_t  data[FAKE_FILE_MAX_BYTES];
    size_t   len;
    int      open_count;                 /* opens not yet closed */

    /* Synced view: what a power cut leaves behind. */
    int      synced_used;
    char     synced_name[GT_FILE_NAME_MAX];
    uint8_t  synced_data[FAKE_FILE_MAX_BYTES];
    size_t   synced_len;
} fake_file;

typedef struct {
    fake_file files[FAKE_FS_MAX_FILES];

    /* Scripted failures: the Nth call (counting from 1) of that op fails
     * with GT_ERR_IO; 0 means never. Only that one call fails. */
    int      fail_open_call;
    int      fail_pwrite_call;
    size_t   pwrite_torn_bytes;          /* bytes the failing pwrite still writes */
    int      fail_fsync_call;
    int      fail_ftruncate_call;
    int      fail_fsync_dir_call;
    int      fail_rename_call;
    int      fail_list_call;
    int      fail_unlink_call;

    /* Call counts, failed calls included. */
    int      open_calls;
    int      pwrite_calls;
    int      fsync_calls;
    int      ftruncate_calls;
    int      fsync_dir_calls;
    int      rename_calls;
    int      unlink_calls;
    int      list_calls;
    int      destroy_calls;

    /* Storage bugs the fake can see; correct storage leaves these at 0. */
    int      bad_handle_calls;           /* an op on a handle that isn't open */
    int      too_big_writes;             /* past FAKE_FILE_MAX_BYTES (a test sizing bug) */
} fake_fs;

/* The slot whose current name is `name`, or -1. */
static inline int fake_fs_find_slot(const fake_fs *fs, const char *name)
{
    for (int slot = 0; slot < FAKE_FS_MAX_FILES; slot++) {
        if (fs->files[slot].used && strcmp(fs->files[slot].name, name) == 0) {
            return slot;
        }
    }
    return -1;
}

/* 1 if `name` can be a file name: not NULL, not empty, fits a row. */
static inline int fake_fs_name_ok(const char *name)
{
    return name != NULL && name[0] != '\0' && strlen(name) < GT_FILE_NAME_MAX;
}

/* The open file behind `handle`, or NULL (and counted as a bug). The
 * handle is the slot number. */
static inline fake_file *fake_fs_handle(fake_fs *fs, int handle)
{
    if (handle < 0 || handle >= FAKE_FS_MAX_FILES || fs->files[handle].open_count == 0) {
        fs->bad_handle_calls++;
        return NULL;
    }
    return &fs->files[handle];
}

/* Test setup: create `name` holding `len` bytes, already fsynced and with
 * its directory entry synced, as if it was left by an earlier run.
 * GT_ERR_INVAL for a bad name, a duplicate or too many bytes; GT_ERR_FULL
 * if every slot is taken. */
static inline gt_status fake_fs_put(fake_fs *fs, const char *name,
                                    const uint8_t *bytes, size_t len)
{
    if (!fake_fs_name_ok(name) || len > FAKE_FILE_MAX_BYTES
        || fake_fs_find_slot(fs, name) >= 0) {
        return GT_ERR_INVAL;
    }
    for (int slot = 0; slot < FAKE_FS_MAX_FILES; slot++) {
        fake_file *file = &fs->files[slot];
        if (!file->used && !file->synced_used) {
            memset(file, 0, sizeof *file);
            file->used = file->synced_used = 1;
            strcpy(file->name, name);
            strcpy(file->synced_name, name);
            if (len > 0) {
                memcpy(file->data, bytes, len);
                memcpy(file->synced_data, bytes, len);
            }
            file->len = file->synced_len = len;
            return GT_OK;
        }
    }
    return GT_ERR_FULL;
}

/* Test inspection: the current view of `name`, or NULL if it doesn't
 * exist. Check ->data and ->len. */
static inline const fake_file *fake_fs_file(const fake_fs *fs, const char *name)
{
    int slot = fake_fs_find_slot(fs, name);
    return slot < 0 ? NULL : &fs->files[slot];
}

/* Open handles not yet closed, summed over all files. A storage that
 * closed cleanly leaves this at 0. */
static inline int fake_fs_open_handles(const fake_fs *fs)
{
    int total = 0;
    for (int slot = 0; slot < FAKE_FS_MAX_FILES; slot++) {
        total += fs->files[slot].open_count;
    }
    return total;
}

/* Pull the plug: every file goes back to its synced contents, every
 * directory entry to its synced state, and every handle is gone (the
 * process died with the power). */
static inline void fake_fs_power_cut(fake_fs *fs)
{
    for (int slot = 0; slot < FAKE_FS_MAX_FILES; slot++) {
        fake_file *file = &fs->files[slot];
        file->used = file->synced_used;
        memcpy(file->name, file->synced_name, sizeof file->name);
        memcpy(file->data, file->synced_data, sizeof file->data);
        file->len = file->synced_len;
        file->open_count = 0;
    }
}

static inline gt_status fake_fs_open(void *ctx, const char *name, unsigned flags,
                                     int *handle_out)
{
    fake_fs *fs = ctx;
    fs->open_calls++;
    if (!fake_fs_name_ok(name) || handle_out == NULL) {
        return GT_ERR_INVAL;
    }
    if (fs->open_calls == fs->fail_open_call) {
        return GT_ERR_IO;
    }
    int slot = fake_fs_find_slot(fs, name);
    if (slot < 0 && (flags & GT_FILE_CREATE)) {
        /* A new file: current only, until fsync and fsync_dir. A slot
         * whose old file was unlinked but not yet synced is still taken. */
        for (int free_slot = 0; free_slot < FAKE_FS_MAX_FILES; free_slot++) {
            fake_file *file = &fs->files[free_slot];
            if (!file->used && !file->synced_used) {
                memset(file, 0, sizeof *file);
                file->used = 1;
                strcpy(file->name, name);
                slot = free_slot;
                break;
            }
        }
    }
    if (slot < 0) {
        return GT_ERR_IO;   /* missing without CREATE, or no free slot */
    }
    fs->files[slot].open_count++;
    *handle_out = slot;
    return GT_OK;
}

static inline gt_status fake_fs_size(void *ctx, int handle, uint64_t *size_out)
{
    fake_file *file = fake_fs_handle(ctx, handle);
    if (file == NULL || size_out == NULL) {
        return file == NULL ? GT_ERR_IO : GT_ERR_INVAL;
    }
    *size_out = file->len;
    return GT_OK;
}

static inline gt_status fake_fs_pread(void *ctx, int handle, uint64_t offset,
                                      uint8_t *buf, size_t len)
{
    fake_file *file = fake_fs_handle(ctx, handle);
    if (file == NULL) {
        return GT_ERR_IO;
    }
    if (buf == NULL) {
        return GT_ERR_INVAL;
    }
    if (offset > file->len || len > file->len - offset) {
        return GT_ERR_IO;   /* a short read */
    }
    memcpy(buf, &file->data[offset], len);
    return GT_OK;
}

/* A write past the current end fills the space between with zeros, as
 * POSIX does. The failing call (fail_pwrite_call) still writes its first
 * pwrite_torn_bytes bytes, like a write the power cut stopped halfway. */
static inline gt_status fake_fs_pwrite(void *ctx, int handle, uint64_t offset,
                                       const uint8_t *buf, size_t len)
{
    fake_fs *fs = ctx;
    fs->pwrite_calls++;
    fake_file *file = fake_fs_handle(fs, handle);
    if (file == NULL) {
        return GT_ERR_IO;
    }
    if (buf == NULL) {
        return GT_ERR_INVAL;
    }
    if (offset > FAKE_FILE_MAX_BYTES || len > FAKE_FILE_MAX_BYTES - offset) {
        fs->too_big_writes++;
        return GT_ERR_IO;
    }
    int failing = fs->pwrite_calls == fs->fail_pwrite_call;
    size_t write_len = failing ? (fs->pwrite_torn_bytes < len ? fs->pwrite_torn_bytes : len) : len;
    if (offset > file->len) {
        memset(&file->data[file->len], 0, offset - file->len);
    }
    memcpy(&file->data[offset], buf, write_len);
    if (offset + write_len > file->len) {
        file->len = offset + write_len;
    }
    return failing ? GT_ERR_IO : GT_OK;
}

static inline gt_status fake_fs_fsync(void *ctx, int handle)
{
    fake_fs *fs = ctx;
    fs->fsync_calls++;
    fake_file *file = fake_fs_handle(fs, handle);
    if (file == NULL) {
        return GT_ERR_IO;
    }
    if (fs->fsync_calls == fs->fail_fsync_call) {
        return GT_ERR_IO;
    }
    memcpy(file->synced_data, file->data, sizeof file->data);
    file->synced_len = file->len;
    return GT_OK;
}

/* Growing a file with ftruncate fills the new bytes with zeros, as POSIX
 * does. */
static inline gt_status fake_fs_ftruncate(void *ctx, int handle, uint64_t len)
{
    fake_fs *fs = ctx;
    fs->ftruncate_calls++;
    fake_file *file = fake_fs_handle(fs, handle);
    if (file == NULL) {
        return GT_ERR_IO;
    }
    if (fs->ftruncate_calls == fs->fail_ftruncate_call) {
        return GT_ERR_IO;
    }
    if (len > FAKE_FILE_MAX_BYTES) {
        fs->too_big_writes++;
        return GT_ERR_IO;
    }
    if (len > file->len) {
        memset(&file->data[file->len], 0, len - file->len);
    }
    file->len = len;
    return GT_OK;
}

static inline void fake_fs_close(void *ctx, int handle)
{
    fake_file *file = fake_fs_handle(ctx, handle);
    if (file != NULL) {
        file->open_count--;
    }
}

/* Every slot's directory state becomes durable: synced_used and
 * synced_name catch up with used and name. File contents are not
 * touched; those need fsync. */
static inline gt_status fake_fs_fsync_dir(void *ctx)
{
    fake_fs *fs = ctx;
    fs->fsync_dir_calls++;
    if (fs->fsync_dir_calls == fs->fail_fsync_dir_call) {
        return GT_ERR_IO;
    }
    for (int slot = 0; slot < FAKE_FS_MAX_FILES; slot++) {
        fake_file *file = &fs->files[slot];
        file->synced_used = file->used;
        memcpy(file->synced_name, file->name, sizeof file->name);
    }
    return GT_OK;
}

/* A file already called new_name is replaced: its name disappears from
 * the directory (its slot frees up once fsync_dir makes that durable). */
static inline gt_status fake_fs_rename(void *ctx, const char *old_name, const char *new_name)
{
    fake_fs *fs = ctx;
    fs->rename_calls++;
    if (!fake_fs_name_ok(old_name) || !fake_fs_name_ok(new_name)) {
        return GT_ERR_INVAL;
    }
    if (fs->rename_calls == fs->fail_rename_call) {
        return GT_ERR_IO;
    }
    int slot = fake_fs_find_slot(fs, old_name);
    if (slot < 0) {
        return GT_ERR_IO;
    }
    int replaced = fake_fs_find_slot(fs, new_name);
    if (replaced >= 0 && replaced != slot) {
        fs->files[replaced].used = 0;
    }
    strcpy(fs->files[slot].name, new_name);
    return GT_OK;
}

static inline gt_status fake_fs_unlink(void *ctx, const char *name)
{
    fake_fs *fs = ctx;
    fs->unlink_calls++;
    if (!fake_fs_name_ok(name)) {
        return GT_ERR_INVAL;
    }
    if (fs->unlink_calls == fs->fail_unlink_call) {
        return GT_ERR_IO;
    }
    int slot = fake_fs_find_slot(fs, name);
    if (slot < 0) {
        return GT_ERR_IO;
    }
    fs->files[slot].used = 0;
    return GT_OK;
}

/* Names in slot order. FAKE_FS_MAX_FILES is below GT_FILE_LIST_MAX, so
 * the fake never returns GT_ERR_FULL. */
static inline gt_status fake_fs_list(void *ctx, char names_out[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX],
                                     size_t *count_out)
{
    fake_fs *fs = ctx;
    fs->list_calls++;
    if (names_out == NULL || count_out == NULL) {
        return GT_ERR_INVAL;
    }
    if (fs->list_calls == fs->fail_list_call) {
        return GT_ERR_IO;
    }
    size_t count = 0;
    for (int slot = 0; slot < FAKE_FS_MAX_FILES; slot++) {
        if (fs->files[slot].used) {
            memcpy(names_out[count], fs->files[slot].name, GT_FILE_NAME_MAX);
            count++;
        }
    }
    *count_out = count;
    return GT_OK;
}

/* Count only: the test owns the fake, so there is nothing to free. */
static inline void fake_fs_destroy(void *ctx)
{
    fake_fs *fs = ctx;
    fs->destroy_calls++;
}

/* Reset `fs` and return the ops that route every call to it. Pass `fs`
 * itself as the ctx: gt_storage_open_ops(fake_fs_ops(&fs), &fs, ...). */
static inline const gt_file_ops *fake_fs_ops(fake_fs *fs)
{
    static const gt_file_ops ops = {
        .open      = fake_fs_open,
        .size      = fake_fs_size,
        .pread     = fake_fs_pread,
        .pwrite    = fake_fs_pwrite,
        .fsync     = fake_fs_fsync,
        .ftruncate = fake_fs_ftruncate,
        .close     = fake_fs_close,
        .fsync_dir = fake_fs_fsync_dir,
        .rename    = fake_fs_rename,
        .unlink    = fake_fs_unlink,
        .list      = fake_fs_list,
        .destroy   = fake_fs_destroy,
    };
    memset(fs, 0, sizeof *fs);
    return &ops;
}

#endif
