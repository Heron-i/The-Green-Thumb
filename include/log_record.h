#ifndef LOG_RECORD_H
#define LOG_RECORD_H

#include <stddef.h>
#include <stdint.h>
#include "ipc.h"

/* On-disk format: see DESIGN.md "In what format". Little-endian, packed. */
#define GT_SEG_MAGIC      "GTLG"
#define GT_SEG_VERSION    1
#define GT_SEG_HEADER_SZ  16
#define GT_REC_MAGIC      0xA7
#define GT_REC_VERSION    1
#define GT_REC_SIZE       24

typedef enum { GT_REC_DATA = 0x01, GT_REC_GAP = 0x02 } gt_rec_type;

typedef struct {
    uint8_t  type;       /* gt_rec_type */
    uint8_t  sensor_id;  /* gt_sensor_id */
    uint32_t seq;
    uint64_t timestamp_us;
    uint32_t value;      /* DATA: raw count; GAP: refused-reading count */
} gt_record;

uint32_t  gt_crc32(const void *buf, size_t len);
gt_status gt_record_encode(const gt_record *rec, uint8_t out[GT_REC_SIZE]);
gt_status gt_record_decode(const uint8_t in[GT_REC_SIZE], gt_record *rec);

#endif
