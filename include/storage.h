#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include "ipc.h"
#include "log_record.h"

#define GT_BATCH_MAX_RECORDS 64   /* N */
#define GT_BATCH_MAX_AGE_S   60   /* T */

typedef struct gt_storage gt_storage;

/* Opens the segment dir and runs recovery before accepting data. */
gt_storage *gt_storage_open(const char *dir);
/* Scan one segment; truncate at the first bad record. */
gt_status   gt_storage_recover(const char *segment_path, size_t *removed_records);
gt_status   gt_storage_append(gt_storage *s, const gt_record *rec);
/* write() + fsync() the batch; a record is committed once this returns OK. */
gt_status   gt_storage_flush(gt_storage *s);
void        gt_storage_close(gt_storage *s);

#endif
