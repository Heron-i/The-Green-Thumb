#ifndef LISTEN_SOCK_H
#define LISTEN_SOCK_H

#include "ipc.h"

/* One listener per socket (Sensor-Pipeline.md, Decisions): the shared
 * helper for every process that listens on a socket in /run/plantpi
 * (hub_listen now; the hub and storage next).
 *
 * The rule: a listener first takes an fcntl write lock on
 * "<sock_path>.lock" and holds it for its whole life. Only the lock holder
 * may remove a leftover socket file and bind a new one; a second copy is
 * told it is already running and touches nothing. The kernel drops the
 * lock when the holder dies for any reason, kill -9 included, so a crash
 * never leaves a stale lock (the lock file itself stays and is reused).
 * This is safer than probing the socket with connect(): two copies
 * started together could both see the file as stale.
 *
 * These helpers never print: they return a status and the caller logs.
 *
 * One fcntl trap to know: a lock belongs to the whole process, not to the
 * descriptor. Taking it twice in one process "succeeds", and closing
 * EITHER descriptor then releases the lock for both. So a process calls
 * gt_listen_lock exactly once and never opens the lock file anywhere
 * else. */

/* Take the lock for sock_path, without waiting.
 *   GT_OK        *lock_fd_out holds the lock: keep it open (and pass it to
 *                gt_listen_release) for as long as the process listens
 *   GT_ERR_IO    *already_running_out is 1 if another process holds the
 *                lock, or 0 if the lock file could not be opened
 *   GT_ERR_INVAL NULL args, or an empty sock_path or one too long for a
 *                socket address
 * *lock_fd_out is untouched on failure; *already_running_out is set (0 or
 * 1) on every call with non-NULL args. */
gt_status gt_listen_lock(const char *sock_path, int *lock_fd_out, int *already_running_out);

/* Call only while holding gt_listen_lock for sock_path: the lock proves no
 * other listener is alive, so a socket file already at sock_path is a
 * leftover from one that died, and is removed. Then make an AF_UNIX
 * SOCK_SEQPACKET socket, bind it to sock_path and listen with `backlog`.
 *   GT_OK        *listen_fd_out is the listening socket
 *   GT_ERR_INVAL NULL args, an empty sock_path or one too long for a
 *                socket address (107 characters), or something other than
 *                a socket at sock_path, which is left untouched
 *   GT_ERR_IO    the socket, bind or listen failed
 * *listen_fd_out is untouched on failure. */
gt_status gt_listen_bind(const char *sock_path, int backlog, int *listen_fd_out);

/* Shut down in the safe order: close the listener, remove the socket
 * file, and close the lock last, so no other copy can bind a new socket
 * while this one's file is still being removed. A descriptor of -1 is
 * skipped (and the socket file is then left alone). */
void      gt_listen_release(const char *sock_path, int listen_fd, int lock_fd);

#endif
