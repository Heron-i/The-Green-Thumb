/* One listener per socket: the instance lock and the bind (see
 * listen_sock.h).
 *
 * Purpose: the shared code every listening process needs before it can
 * accept connections in /run/plantpi: hold "<socket>.lock" for its whole
 * life, and only then replace a socket file left behind by a listener
 * that died. Moved here from hub_listen_main.c (Step 23 of the IPC phase)
 * so the hub and storage follow the same rule.
 *
 * Linux model: the lock is an fcntl write lock (F_SETLK, never waiting).
 * The socket is AF_UNIX SOCK_SEQPACKET, so the kernel keeps message
 * boundaries. These functions never print; on failure they leave errno as
 * the failing call set it (restoring it around any clean-up close()), so
 * a caller can still print strerror(errno). */
#include "listen_sock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* The longest socket path: sun_path is a fixed 108-byte array that must
 * also hold the NUL. */
#define MAX_SOCK_PATH (sizeof(((struct sockaddr_un *)0)->sun_path) - 1)

/* 1 if sock_path can be a socket address: not empty, and short enough. */
static int sock_path_ok(const char *sock_path)
{
    size_t path_len = strlen(sock_path);
    return path_len > 0 && path_len <= MAX_SOCK_PATH;
}

/* Close fd without letting close() overwrite the errno of the failure
 * the caller is about to report. */
static void close_keep_errno(int fd)
{
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
}

/* `struct flock` describes the lock: F_WRLCK is an exclusive (write) lock,
 * and l_start = l_len = 0 covers the whole file. F_SETLK fails at once
 * (EACCES or EAGAIN) instead of waiting if another process holds it. The
 * kernel drops the lock when the holder exits for any reason, kill -9
 * included; the lock file itself stays and is reused. */
gt_status gt_listen_lock(const char *sock_path, int *lock_fd_out, int *already_running_out)
{
    if (sock_path == NULL || lock_fd_out == NULL || already_running_out == NULL) {
        return GT_ERR_INVAL;
    }
    *already_running_out = 0;
    if (!sock_path_ok(sock_path)) {
        return GT_ERR_INVAL;
    }
    char lock_path[128];   /* a valid socket path (<= 107) + ".lock" fits */
    snprintf(lock_path, sizeof lock_path, "%s.lock", sock_path);

    int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_fd < 0) {
        return GT_ERR_IO;
    }
    struct flock lock;
    memset(&lock, 0, sizeof lock);
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (fcntl(lock_fd, F_SETLK, &lock) != 0) {
        *already_running_out = errno == EACCES || errno == EAGAIN;
        close_keep_errno(lock_fd);
        return GT_ERR_IO;
    }
    *lock_fd_out = lock_fd;
    return GT_OK;
}

/* bind() refuses a path that already exists (EADDRINUSE), so a socket
 * file left by a listener killed with kill -9 is removed first. lstat()
 * describes the path itself without following links, and S_ISSOCK checks
 * it really is a socket: any other kind of file is left alone. Safe only
 * under the lock: the lock proves no other listener is alive, so the old
 * file can't belong to a running one. SOCK_CLOEXEC closes the socket
 * automatically if this process ever exec()s another program. */
gt_status gt_listen_bind(const char *sock_path, int backlog, int *listen_fd_out)
{
    if (sock_path == NULL || listen_fd_out == NULL || !sock_path_ok(sock_path)) {
        return GT_ERR_INVAL;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);

    struct stat info;
    if (lstat(sock_path, &info) == 0) {
        if (!S_ISSOCK(info.st_mode)) {
            return GT_ERR_INVAL;   /* not ours to remove */
        }
        unlink(sock_path);
    }

    int listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        return GT_ERR_IO;
    }
    if (bind(listen_fd, (const struct sockaddr *)&addr, sizeof addr) != 0
        || listen(listen_fd, backlog) != 0) {
        close_keep_errno(listen_fd);
        return GT_ERR_IO;
    }
    *listen_fd_out = listen_fd;
    return GT_OK;
}

/* The socket file is removed while the lock is still held, so a new copy
 * can't bind its own socket in between and then lose it to this unlink. */
void gt_listen_release(const char *sock_path, int listen_fd, int lock_fd)
{
    if (listen_fd >= 0) {
        close(listen_fd);
        if (sock_path != NULL) {
            unlink(sock_path);
        }
    }
    if (lock_fd >= 0) {
        close(lock_fd);
    }
}
