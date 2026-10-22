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

/* 16 B sensor -> hub -> storage message over AF_UNIX sockets in /run/plantpi. */
#define GT_MSG_SIZE 16

typedef struct {
    uint8_t  sensor_id;   /* gt_sensor_id */
    uint32_t counter;     /* sensor-side counter, used to detect gaps */
    uint32_t seq;         /* hub-assigned; 0 before the hub stamps it */
    uint32_t value;       /* raw count */
} gt_msg;

gt_status gt_msg_encode(const gt_msg *m, uint8_t out[GT_MSG_SIZE]);
gt_status gt_msg_decode(const uint8_t in[GT_MSG_SIZE], gt_msg *m);

/* Client side: reconnect with 100 ms -> 2 s backoff; bounded send queue. */
typedef struct gt_ipc_client gt_ipc_client;

gt_ipc_client *gt_ipc_client_open(const char *sock_path);
gt_status      gt_ipc_client_send(gt_ipc_client *c, const gt_msg *m);
void           gt_ipc_client_close(gt_ipc_client *c);

#endif
