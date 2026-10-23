/* 16 B sensor -> hub message encode/decode (see ipc.h for the layout).
 *
 * Purpose: turns a gt_msg into the exact bytes sent over hub.sock and back.
 * Fields are written one byte at a time in little-endian order instead of
 * copying the struct, because the compiler may add padding between struct
 * fields and the Pi and a test machine need not agree on it. Decode is the
 * hub's first check (DESIGN.md failure-mode table, Hub: "Length/magic/
 * version check"): anything it rejects is logged as ipc_bad_message.
 *
 * Pure byte packing: no sockets. The client that sends these bytes goes in
 * src/ipc_client.c. */
#include "ipc.h"

#include <stddef.h>

/* Write `value` into out[0..3], lowest byte first. */
static void put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value);
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
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

/* Whether type and sensor_id are allowed on the sensor -> hub hop: DATA or
 * GAP, from the light or soil sensor. GT_SENSOR_ALL is only for storage GAP
 * records, never from one sensor. Shared so encode refuses exactly what
 * decode would reject. */
static int fields_valid(uint8_t type, uint8_t sensor_id)
{
    int type_ok = type == GT_MSG_DATA || type == GT_MSG_GAP;
    int sensor_ok = sensor_id == GT_SENSOR_LIGHT || sensor_id == GT_SENSOR_SOIL;
    return type_ok && sensor_ok;
}

/* Pack `message` into the 16 wire bytes in `out`. Everything is checked
 * before the first byte is written, so a rejected message leaves `out`
 * exactly as it was. GT_ERR_INVAL on NULL args or a bad type/sensor_id. */
gt_status gt_msg_encode(const gt_msg *message, uint8_t out[GT_MSG_SIZE])
{
    if (message == NULL || out == NULL) {
        return GT_ERR_INVAL;
    }
    if (!fields_valid(message->type, message->sensor_id)) {
        return GT_ERR_INVAL;
    }

    out[0] = GT_MSG_MAGIC;
    out[1] = GT_MSG_VERSION;
    out[2] = message->type;
    out[3] = message->sensor_id;
    put_u32_le(&out[4], message->counter);
    put_u32_le(&out[8], message->seq);
    put_u32_le(&out[12], message->value);
    return GT_OK;
}

/* Unpack 16 wire bytes from `in` into `message`. The header is checked
 * first and `message` is written only once every check has passed, so the
 * hub never sees a half-decoded message. GT_ERR_INVAL on NULL args (a
 * caller bug); GT_ERR_CORRUPT on bad magic, version, type or sensor_id
 * (bad bytes: the hub drops the connection so the sensor resyncs). */
gt_status gt_msg_decode(const uint8_t in[GT_MSG_SIZE], gt_msg *message)
{
    if (in == NULL || message == NULL) {
        return GT_ERR_INVAL;
    }
    if (in[0] != GT_MSG_MAGIC || in[1] != GT_MSG_VERSION || !fields_valid(in[2], in[3])) {
        return GT_ERR_CORRUPT;
    }

    message->type = in[2];
    message->sensor_id = in[3];
    message->counter = get_u32_le(&in[4]);
    message->seq = get_u32_le(&in[8]);
    message->value = get_u32_le(&in[12]);
    return GT_OK;
}
