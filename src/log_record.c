/* 24 B record encode/decode and its CRC-32 (see log_record.h for the layout).
 *
 * Purpose: turns a gt_record into the exact bytes that go on disk and over
 * storage.sock (hub -> storage), and back. As in ipc_msg.c, fields are
 * written one byte at a time in little-endian order instead of copying the
 * struct, so compiler padding never reaches the disk. The CRC over bytes
 * 0-19 is what lets the startup recovery scan (DESIGN.md "What happens when
 * the power dies mid-write") spot a torn or damaged record and truncate there.
 *
 * Pure byte packing and arithmetic: no files or sockets. */
#include "log_record.h"

#include <stddef.h>

/* The standard CRC-32 polynomial (zlib, Ethernet, PNG) in reflected form:
 * bits are processed lowest first, so the polynomial is bit-reversed. */
#define CRC32_POLY_REFLECTED 0xEDB88320u

/* Write `value` into out[0..3], lowest byte first. */
static void put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value);
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

/* Write a u64 as two little-endian u32 halves, low half first, so out[0..7]
 * is lowest byte first overall. */
static void put_u64_le(uint8_t *out, uint64_t value)
{
    put_u32_le(&out[0], (uint32_t)(value));
    put_u32_le(&out[4], (uint32_t)(value >> 32));
}

/* Read in[0..3], lowest byte first. Each byte is widened to uint32_t
 * before shifting, so a shift by 24 can't overflow an int. */
static uint32_t get_u32_le(const uint8_t *in)
{
    return (uint32_t)in[0]
         | ((uint32_t)in[1] << 8)
         | ((uint32_t)in[2] << 16)
         | ((uint32_t)in[3] << 24);
}

/* Read a u64 from two little-endian u32 halves, low half first. The high
 * half is widened to uint64_t before the 32-bit shift, because shifting a
 * 32-bit value by 32 is undefined in C. */
static uint64_t get_u64_le(const uint8_t *in)
{
    return (uint64_t)get_u32_le(&in[0])
         | ((uint64_t)get_u32_le(&in[4]) << 32);
}

/* Whether type and sensor_id are allowed in a record: DATA or GAP, from the
 * light or soil sensor. GT_SENSOR_ALL is allowed only in a GAP, because
 * storage records one gap for every sensor when its own queue is full; a
 * DATA reading always comes from one sensor. Shared so encode refuses
 * exactly what decode would reject. */
static int fields_valid(uint8_t type, uint8_t sensor_id)
{
    int type_ok = type == GT_REC_DATA || type == GT_REC_GAP;
    int one_sensor = sensor_id == GT_SENSOR_LIGHT || sensor_id == GT_SENSOR_SOIL;
    int all_sensors_gap = type == GT_REC_GAP && sensor_id == GT_SENSOR_ALL;
    return type_ok && (one_sensor || all_sensors_gap);
}

/* Standard CRC-32, one bit at a time (no lookup table: a record is only 20
 * bytes, so speed doesn't matter). The running value starts at 0xFFFFFFFF
 * and is inverted at the end, so the empty input gives 0. In C, a
 * `const void *` converts to `const uint8_t *` without a cast, which lets
 * callers pass any buffer. */
uint32_t gt_crc32(const void *buf, size_t len)
{
    const uint8_t *bytes = buf;
    uint32_t crc = 0xFFFFFFFFu;

    /* invariant: crc is the (not yet inverted) CRC of bytes[0..byte_index) */
    for (size_t byte_index = 0; byte_index < len; byte_index++) {
        crc ^= bytes[byte_index];
        /* invariant: the low bit_count bits of this byte have been divided out */
        for (int bit_count = 0; bit_count < 8; bit_count++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ CRC32_POLY_REFLECTED;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Bytes 0-19 are covered by the CRC; the CRC itself sits at 20-23. */
#define CRC_COVERED_LEN 20

/* Pack `rec` into the 24 bytes in `out`, then seal them with the CRC of
 * bytes 0-19. Everything is checked before the first byte is written, so a
 * rejected record leaves `out` exactly as it was. GT_ERR_INVAL on NULL args
 * or a bad type/sensor_id. */
gt_status gt_record_encode(const gt_record *rec, uint8_t out[GT_REC_SIZE])
{
    if (rec == NULL || out == NULL) {
        return GT_ERR_INVAL;
    }
    if (!fields_valid(rec->type, rec->sensor_id)) {
        return GT_ERR_INVAL;
    }

    out[0] = GT_REC_MAGIC;
    out[1] = rec->type;          /* type before version: the reverse of the message */
    out[2] = GT_REC_VERSION;
    out[3] = rec->sensor_id;
    put_u32_le(&out[4], rec->seq);
    put_u64_le(&out[8], rec->timestamp_us);
    put_u32_le(&out[16], rec->value);
    put_u32_le(&out[20], gt_crc32(out, CRC_COVERED_LEN));
    return GT_OK;
}

/* Unpack 24 bytes from `in` into `rec`. The CRC is checked first, so no
 * field of a damaged record is trusted; then magic, version, type and
 * sensor_id. `rec` is written only once every check has passed.
 * GT_ERR_INVAL on NULL args (a caller bug); GT_ERR_CORRUPT on any bad byte
 * (storage refuses it; recovery truncates the log at the first such record). */
gt_status gt_record_decode(const uint8_t in[GT_REC_SIZE], gt_record *rec)
{
    if (in == NULL || rec == NULL) {
        return GT_ERR_INVAL;
    }
    if (get_u32_le(&in[20]) != gt_crc32(in, CRC_COVERED_LEN)) {
        return GT_ERR_CORRUPT;
    }
    if (in[0] != GT_REC_MAGIC || in[2] != GT_REC_VERSION || !fields_valid(in[1], in[3])) {
        return GT_ERR_CORRUPT;
    }

    rec->type = in[1];
    rec->sensor_id = in[3];
    rec->seq = get_u32_le(&in[4]);
    rec->timestamp_us = get_u64_le(&in[8]);
    rec->value = get_u32_le(&in[16]);
    return GT_OK;
}
