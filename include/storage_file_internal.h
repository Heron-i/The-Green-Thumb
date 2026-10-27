#ifndef STORAGE_FILE_INTERNAL_H
#define STORAGE_FILE_INTERNAL_H

/* The Linux file layer's private parts, shared by its two files:
 * storage_file_io.c (operations on an open file) and storage_file.c (the
 * directory, and the gt_file_ops table that routes to both). Only those
 * two include this header; storage reaches these ops through the table
 * from gt_file_ops_linux, never by name.
 *
 * Each op takes the table's ctx so its type matches the gt_file_ops slot,
 * but ignores it: a handle is the file's real descriptor, which is all
 * these calls need. */
#include <stddef.h>
#include <stdint.h>

#include "storage_io.h"

gt_status gt_file_size_linux(void *ctx, int handle, uint64_t *size_out);
gt_status gt_file_pread_linux(void *ctx, int handle, uint64_t offset, uint8_t *buf, size_t len);
gt_status gt_file_pwrite_linux(void *ctx, int handle, uint64_t offset, const uint8_t *buf,
                               size_t len);
gt_status gt_file_fsync_linux(void *ctx, int handle);
gt_status gt_file_ftruncate_linux(void *ctx, int handle, uint64_t len);
void      gt_file_close_linux(void *ctx, int handle);

#endif
