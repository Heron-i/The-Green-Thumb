#ifndef STORAGE_IO_H
#define STORAGE_IO_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"

/* File seam for storage, the same idea as gt_i2c_ops (i2c_bus.h) and
 * gt_sock_ops (ipc_sock.h).
 *
 * Storage touches the log directory (/var/lib/plantpi) only through these
 * ops, so tests can swap in tests/fake_file.h: an in-memory directory
 * that can fail a chosen write or fsync and can simulate a power cut. The
 * Linux impl (src/storage_file.c, Mechanism D Step 12a) is the only file
 * that makes file system calls.
 *
 * The ctx stands for one directory, and files are named relative to it
 * ("log-20261026.bin"). An open file is identified by an `int` handle,
 * like a Linux file descriptor: a small number the ops hand out on open
 * and take back on close. Reads and writes take an explicit byte offset
 * (like pread/pwrite), so there is no hidden "current position" to keep
 * track of.
 *
 * Every op returns GT_OK or GT_ERR_IO (any failure the caller can't fix:
 * missing file, permission, a disk error), and GT_ERR_INVAL for a bad
 * argument such as a name too long for GT_FILE_NAME_MAX. */
#define GT_FILE_NAME_MAX 32    /* bytes, including the NUL: "log-YYYYMMDD.bin.corrupt" fits */
#define GT_FILE_LIST_MAX 128   /* names list() can return: 30 days of segments plus spares */

/* Flags for open(). Bits, combined with |, like the GT_IPC_EV_* flags. */
#define GT_FILE_CREATE 0x1u    /* create an empty file if the name doesn't exist */

typedef struct {
    /* Open `name` for reading and writing. Without GT_FILE_CREATE a
     * missing file is GT_ERR_IO. A new file's name is not durable until
     * fsync_dir. */
    gt_status (*open)(void *ctx, const char *name, unsigned flags, int *handle_out);
    /* The file's current length in bytes. */
    gt_status (*size)(void *ctx, int handle, uint64_t *size_out);
    /* Read exactly `len` bytes at `offset`. A short read (past the end) is
     * GT_ERR_IO. */
    gt_status (*pread)(void *ctx, int handle, uint64_t offset, uint8_t *buf, size_t len);
    /* Write all `len` bytes at `offset`. On GT_ERR_IO some of the bytes
     * may have been written (a torn write): the caller truncates back. */
    gt_status (*pwrite)(void *ctx, int handle, uint64_t offset, const uint8_t *buf, size_t len);
    /* Make the file's contents durable: once this returns GT_OK, a power
     * cut can't undo any write before it. */
    gt_status (*fsync)(void *ctx, int handle);
    /* Cut the file to `len` bytes. */
    gt_status (*ftruncate)(void *ctx, int handle, uint64_t len);
    /* Give the handle back. Safe to call once per successful open. */
    void      (*close)(void *ctx, int handle);
    /* Make every create, rename and unlink in the directory durable. */
    gt_status (*fsync_dir)(void *ctx);
    /* Rename `old_name` to `new_name`, replacing new_name if it exists. */
    gt_status (*rename)(void *ctx, const char *old_name, const char *new_name);
    /* Delete `name`. */
    gt_status (*unlink)(void *ctx, const char *name);
    /* Copy up to GT_FILE_LIST_MAX file names into names_out, in no
     * particular order (callers sort if they need to). `names_out` is an
     * array of GT_FILE_LIST_MAX rows, each a GT_FILE_NAME_MAX-byte string.
     * Names too long for a row are skipped, since storage never makes them.
     * More than GT_FILE_LIST_MAX names is GT_ERR_FULL, with the first
     * GT_FILE_LIST_MAX filled in. */
    gt_status (*list)(void *ctx, char names_out[GT_FILE_LIST_MAX][GT_FILE_NAME_MAX],
                      size_t *count_out);
    /* Free ctx. Called exactly once, by the storage close. */
    void      (*destroy)(void *ctx);
} gt_file_ops;

/* Linux impl (src/storage_file.c): opens the directory `dir` (e.g.
 * "/var/lib/plantpi") once and keeps it open in the ctx; every op then
 * works on names inside it. A name containing '/' or equal to "." or ".."
 * is GT_ERR_INVAL, so storage can never reach outside its directory.
 * GT_ERR_INVAL for a NULL/empty dir; GT_ERR_IO if dir can't be opened (it
 * must already exist: making /var/lib/plantpi is storage_main's job);
 * GT_ERR_NOMEM if the ctx can't be allocated. Outputs are untouched on
 * failure. The two outputs are pointer-to-pointer, as in
 * gt_ipc_sock_linux (ipc_sock.h). */
gt_status gt_file_ops_linux(const char *dir, const gt_file_ops **ops_out, void **ctx_out);

#endif
