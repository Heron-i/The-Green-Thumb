#ifndef IPC_H
#define IPC_H

#include <stdint.h>
/* Shared types: sensor IDs (IPC message and on-disk record) and status codes. */
typedef enum {
    GT_SENSOR_ALL   = 0,
    GT_SENSOR_LIGHT = 1,
    GT_SENSOR_SOIL  = 2
} gt_sensor_id;

/* 0 on success, negative on failure. */
typedef enum {
    GT_OK          =  0,
    GT_ERR_IO      = -1,
    GT_ERR_INVAL   = -2,
    GT_ERR_NOMEM   = -3,
    GT_ERR_CORRUPT = -4,
    GT_ERR_FULL    = -5
} gt_status;

/* 16 B sensor -> hub message over AF_UNIX sockets in /run/plantpi.
 * (Hub -> storage sends the 24 B record instead: see log_record.h.)
 *
 * Wire layout, little-endian, written byte by byte (never by struct copy,
 * so compiler padding can't leak onto the wire):
 *   off 0  magic      GT_MSG_MAGIC
 *   off 1  version    GT_MSG_VERSION
 *   off 2  type       GT_MSG_DATA / GT_MSG_GAP
 *   off 3  sensor_id  GT_SENSOR_LIGHT / GT_SENSOR_SOIL (ALL is not valid here)
 *   off 4  counter    u32
 *   off 8  seq        u32
 *   off 12 value      u32
 * Magic and version are written by encode and checked by decode, so they
 * are not struct fields. */
#define GT_MSG_SIZE    16
#define GT_MSG_MAGIC   0xA6   /* distinct from the on-disk record's 0xA7 */
#define GT_MSG_VERSION 1

/* Same values as gt_rec_type in log_record.h. */
#define GT_MSG_DATA 0x01
#define GT_MSG_GAP  0x02

typedef struct {
    uint8_t  type;        /* GT_MSG_DATA or GT_MSG_GAP */
    uint8_t  sensor_id;   /* gt_sensor_id */
    uint32_t counter;     /* sensor-side counter; GAP: first refused counter */
    uint32_t seq;         /* hub-assigned; 0 before the hub stamps it */
    uint32_t value;       /* DATA: raw count; GAP: refused-reading count */
} gt_msg;

gt_status gt_msg_encode(const gt_msg *message, uint8_t out[GT_MSG_SIZE]);
gt_status gt_msg_decode(const uint8_t in[GT_MSG_SIZE], gt_msg *message);

/* Client side: reconnect with 100 ms -> 2 s backoff; bounded send queue. */
typedef struct gt_ipc_client gt_ipc_client;

gt_ipc_client *gt_ipc_client_open(const char *sock_path);
gt_status      gt_ipc_client_send(gt_ipc_client *c, const gt_msg *m);
void           gt_ipc_client_close(gt_ipc_client *c);

#endif
