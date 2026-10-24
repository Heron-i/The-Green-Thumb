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

/* Receive side (the hub): check what one recv() returned, then decode it.
 * `len` is recv()'s return value, so it is signed (`long`): -1 means
 * recv() failed and 0 means the sensor closed the connection, and both
 * return GT_ERR_IO. Any other length but GT_MSG_SIZE is GT_ERR_CORRUPT
 * (logged as ipc_bad_message). The hub recv()s into a GT_MSG_SIZE + 1
 * buffer, because SOCK_SEQPACKET silently cuts off a message that is
 * longer than the buffer; the extra byte makes an oversized message show
 * up as 17. NULL args are GT_ERR_INVAL. Otherwise the result is
 * gt_msg_decode's, and `message` is untouched on any failure. */
gt_status gt_ipc_frame_decode(const uint8_t *buf, long len, gt_msg *message);

/* Client side (DESIGN.md failure-mode table, Sensor -> hub row): each
 * sensor process sends its messages to hub.sock through one client. It
 * buffers up to GT_IPC_RING_SLOTS messages while the hub is down,
 * reconnects with a doubling backoff, and after an overflow sends one GAP
 * message covering the readings it had to refuse. The client never logs:
 * it returns event flags and the caller logs them (like gt_health_record).
 * Rules: Sensor-Pipeline.md -> "IPC client: connection and ring rules". */
#define GT_IPC_RING_SLOTS      64     /* 64 x 16 B = about 1 KB */
#define GT_IPC_BACKOFF_MIN_MS  100    /* first wait; reset after a connect */
#define GT_IPC_BACKOFF_MAX_MS  2000   /* the wait doubles up to this */

/* Event flags. Each event is one bit, so several can be reported at once:
 * the client ORs them together (`events |= GT_IPC_EV_DISCONNECT`), and the
 * caller tests one with `&` (`if (events & GT_IPC_EV_OVERFLOW)`). They are
 * `unsigned` because bit operations on unsigned values have no sign bit to
 * get in the way. The caller logs the matching event name. */
#define GT_IPC_EV_DISCONNECT 0x1u   /* lost the hub: log ipc_disconnect */
#define GT_IPC_EV_RECONNECT  0x2u   /* back after a loss or failed attempt: ipc_reconnect */
#define GT_IPC_EV_OVERFLOW   0x4u   /* ring full, readings refused: ipc_overflow */

/* Counters for tests and for log key=value fields. `queued` is how many
 * frames are waiting right now; the other three are totals since open. */
typedef struct {
    uint32_t queued;       /* frames in the ring (0..GT_IPC_RING_SLOTS) */
    uint32_t sent;         /* frames the kernel accepted */
    uint32_t refused;      /* readings refused because the ring was full */
    uint32_t reconnects;   /* connects that followed a loss or failed attempt */
} gt_ipc_stats;

typedef struct gt_ipc_client gt_ipc_client;

/* A client for the hub socket at `sock_path` (e.g. "/run/plantpi/hub.sock").
 * Does not connect: a hub that isn't up yet is not an error, and the first
 * send or pump tries. NULL for a NULL/empty path, one too long for a
 * socket address (108 B), or no memory. Tests use gt_ipc_client_open_ops
 * (ipc_sock.h) instead. */
gt_ipc_client *gt_ipc_client_open(const char *sock_path);
/* Queue `message`, then connect (if a retry is due) and send what's queued,
 * oldest first. now_ms is a monotonic clock in ms, passed in so tests need
 * no clock. Never blocks.
 *   GT_OK        sent, or buffered until the hub is back
 *   GT_ERR_FULL  refused and counted (the ring is full); a GAP covers it
 *   GT_ERR_INVAL NULL client/message, or a message gt_msg_encode rejects;
 *                nothing is queued
 * Losing the hub is not an error here; it's reported in *events_out (the
 * GT_IPC_EV_* flags, set to 0 first; pass NULL to ignore them). */
gt_status      gt_ipc_client_send(gt_ipc_client *client, const gt_msg *message,
                                  uint64_t now_ms, unsigned *events_out);
/* The connect-and-send half of gt_ipc_client_send, with no new message.
 * The sensor loop calls it every tick, so the ring still drains while the
 * sensor is offline and producing nothing. GT_OK, or GT_ERR_INVAL for a
 * NULL client. */
gt_status      gt_ipc_client_pump(gt_ipc_client *client, uint64_t now_ms,
                                  unsigned *events_out);
/* Copy the client's counters into *stats_out. GT_ERR_INVAL for NULL args. */
gt_status      gt_ipc_client_stats(const gt_ipc_client *client, gt_ipc_stats *stats_out);
/* Close the connection and free the client. Sends nothing more: frames
 * still in the ring are dropped. NULL is a no-op. */
void           gt_ipc_client_close(gt_ipc_client *client);

#endif
