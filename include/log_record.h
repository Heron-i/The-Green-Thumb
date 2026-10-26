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

/* 24 B record: written to disk, and also sent hub -> storage unchanged.
 *
 * Wire layout, little-endian, written byte by byte (never by struct copy,
 * so compiler padding can't leak onto the wire or the disk):
 *   off 0  magic      GT_REC_MAGIC
 *   off 1  type       GT_REC_DATA / GT_REC_GAP
 *   off 2  version    GT_REC_VERSION
 *   off 3  sensor_id  LIGHT / SOIL; a GAP may also use GT_SENSOR_ALL
 *   off 4  seq        u32, hub-assigned
 *   off 8  timestamp  u64, hub receive time (GAP: gap start), us since epoch
 *   off 16 value      u32
 *   off 20 crc32      u32, CRC-32 over bytes 0-19
 * Note: type and version are in the opposite order from the 16 B message
 * in ipc.h. Magic, version and CRC are not struct fields. */
typedef struct {
    uint8_t  type;       /* gt_rec_type */
    uint8_t  sensor_id;  /* gt_sensor_id */
    uint32_t seq;
    uint64_t timestamp_us;
    uint32_t value;      /* DATA: raw count; GAP: refused-reading count */
} gt_record;

/* 16 B segment header: written once at the start of each segment file
 * (DESIGN.md "Segment header"), before any record.
 *
 * Wire layout, little-endian, written byte by byte like the record:
 *   off 0  magic        GT_SEG_MAGIC, the 4 ASCII bytes "GTLG" (no NUL)
 *   off 4  version      u16, GT_SEG_VERSION
 *   off 6  record_size  u16, GT_REC_SIZE
 *   off 8  created_at   u64, us since epoch
 * The header has no CRC: recovery trusts it only if magic, version and
 * record_size all match. Magic, version and record_size are not struct
 * fields, because they are always the same. */
typedef struct {
    uint64_t created_at_us;
} gt_seg_header;

/* Write `header` as 16 bytes into `out`. GT_ERR_INVAL for NULL args;
 * `out` is untouched on failure. */
gt_status gt_seg_header_encode(const gt_seg_header *header, uint8_t out[GT_SEG_HEADER_SZ]);

/* Read 16 bytes into `header`. GT_ERR_CORRUPT for a bad magic, version or
 * record_size, and GT_ERR_INVAL for NULL args; `header` is untouched on
 * failure. */
gt_status gt_seg_header_decode(const uint8_t in[GT_SEG_HEADER_SZ], gt_seg_header *header);

/* Standard CRC-32 (the one zlib and Ethernet use) over `len` bytes of `buf`.
 * `const void *` means "a pointer to bytes of any type, read-only". */
uint32_t  gt_crc32(const void *buf, size_t len);

/* Write `rec` as 24 bytes into `out`, including magic, version and CRC.
 * Returns GT_ERR_INVAL for NULL args, an unknown type, or a bad sensor_id;
 * `out` is untouched on failure. */
gt_status gt_record_encode(const gt_record *rec, uint8_t out[GT_REC_SIZE]);

/* Read 24 bytes into `rec`. Returns GT_ERR_CORRUPT for a bad magic, version,
 * type, sensor_id or CRC, and GT_ERR_INVAL for NULL args; `rec` is untouched
 * on failure. */
gt_status gt_record_decode(const uint8_t in[GT_REC_SIZE], gt_record *rec);

#endif
