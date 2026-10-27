/* Tests for binding a listener (src/listen_sock.c): gt_listen_bind and
 * gt_listen_release, used while holding gt_listen_lock.
 *
 * Purpose: prove a listener comes up on a real AF_UNIX SOCK_SEQPACKET
 * socket that a client can connect to; a socket file left behind by a
 * listener that was killed is replaced, not tripped over (bind() alone
 * would fail with EADDRINUSE); anything at the path that is not a socket
 * is refused and left untouched; and release removes the socket file and
 * frees the lock, so a restarted copy can take over.
 *
 * Each test uses its own mkdtemp directory under /tmp (Windows-mounted
 * drives in WSL2 don't reliably support socket files) and removes it
 * after. Nothing blocks: a connect to a listening AF_UNIX socket completes
 * at once. Run under make test / asan / memcheck. Implements LBD-01
 * through LBD-06.
 *
 * Note: macOS does not support SOCK_SEQPACKET for AF_UNIX, so these run on
 * Linux (WSL and the Pi) only, like test_ipc_loopback.c. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.h"
#include "listen_sock.h"

/* One test's directory and the socket path in it. */
typedef struct {
    char dir[32];    /* "/tmp/gt-lbd-XXXXXX", the X's filled in by mkdtemp */
    char path[64];   /* dir + "/test.sock" */
} bind_dir;

/* Make a fresh directory. Returns 1 on success, 0 on failure. */
static int bind_dir_init(bind_dir *fixture)
{
    strcpy(fixture->dir, "/tmp/gt-lbd-XXXXXX");
    if (mkdtemp(fixture->dir) == NULL) {
        return 0;
    }
    snprintf(fixture->path, sizeof fixture->path, "%s/test.sock", fixture->dir);
    return 1;
}

/* Remove whatever is at the socket path, the lock file and the directory. */
static void bind_dir_remove(const bind_dir *fixture)
{
    char lock_path[80];
    snprintf(lock_path, sizeof lock_path, "%s.lock", fixture->path);
    unlink(fixture->path);
    unlink(lock_path);
    rmdir(fixture->dir);
}

/* `struct sockaddr_un` is the AF_UNIX address type: a family field plus
 * the socket file's path. Fill one for `path`. */
static void fill_address(struct sockaddr_un *addr, const char *path)
{
    memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    strncpy(addr->sun_path, path, sizeof addr->sun_path - 1);
}

/* 1 if a client can connect to the socket at `path` right now. */
static int client_can_connect(const char *path)
{
    struct sockaddr_un addr;
    fill_address(&addr, path);
    int client_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (client_fd < 0) {
        return 0;
    }
    int connected = connect(client_fd, (const struct sockaddr *)&addr, sizeof addr) == 0;
    close(client_fd);
    return connected;
}

/* 1 if `path` exists and is a socket file. lstat describes the path itself
 * (without following a symbolic link); S_ISSOCK tests its type. */
static int is_socket_file(const char *path)
{
    struct stat info;
    return lstat(path, &info) == 0 && S_ISSOCK(info.st_mode);
}

/* Take the lock for the fixture's path, checking it works. */
static int take_lock(const bind_dir *fixture)
{
    int lock_fd = -1, already_running = 0;
    CHECK_EQ(gt_listen_lock(fixture->path, &lock_fd, &already_running), GT_OK);
    return lock_fd;
}

/* ---- LBD-01: a listener comes up and releases ----
 * Bind makes a socket file a client can connect to; release removes it. */
static void test_lbd01_listen_and_release(void)
{
    bind_dir fixture;
    CHECK(bind_dir_init(&fixture));
    int lock_fd = take_lock(&fixture);
    int listen_fd = -1;

    CHECK_EQ(gt_listen_bind(fixture.path, 4, &listen_fd), GT_OK);
    CHECK(listen_fd >= 0);
    CHECK(is_socket_file(fixture.path));
    CHECK(client_can_connect(fixture.path));

    gt_listen_release(fixture.path, listen_fd, lock_fd);
    CHECK(access(fixture.path, F_OK) != 0);
    bind_dir_remove(&fixture);
}

/* ---- LBD-02: a leftover socket file is replaced ----
 * A listener that bound and was killed leaves its socket file behind
 * (here: bind, then close without unlinking). A new bind under the lock
 * removes it and succeeds, and clients reach the new listener. */
static void test_lbd02_leftover_socket_replaced(void)
{
    bind_dir fixture;
    CHECK(bind_dir_init(&fixture));
    struct sockaddr_un addr;
    fill_address(&addr, fixture.path);
    int old_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    CHECK(old_fd >= 0);
    CHECK_EQ(bind(old_fd, (const struct sockaddr *)&addr, sizeof addr), 0);
    close(old_fd);
    CHECK(is_socket_file(fixture.path));
    CHECK(!client_can_connect(fixture.path));   /* nobody listening on the old file */

    int lock_fd = take_lock(&fixture);
    int listen_fd = -1;
    CHECK_EQ(gt_listen_bind(fixture.path, 4, &listen_fd), GT_OK);
    CHECK(client_can_connect(fixture.path));

    gt_listen_release(fixture.path, listen_fd, lock_fd);
    bind_dir_remove(&fixture);
}

/* ---- LBD-03: a non-socket file is left alone ----
 * A regular file at the path is refused with GT_ERR_INVAL: storage or the
 * hub must never delete something it didn't make. It is still there,
 * unchanged, and listen_fd is untouched. */
static void test_lbd03_regular_file_left_alone(void)
{
    bind_dir fixture;
    CHECK(bind_dir_init(&fixture));
    int file_fd = open(fixture.path, O_WRONLY | O_CREAT, 0644);
    CHECK(file_fd >= 0);
    CHECK(write(file_fd, "keep", 4) == 4);
    close(file_fd);

    int lock_fd = take_lock(&fixture);
    int listen_fd = 77;
    CHECK_EQ(gt_listen_bind(fixture.path, 4, &listen_fd), GT_ERR_INVAL);
    CHECK_EQ(listen_fd, 77);

    struct stat info;
    CHECK_EQ(lstat(fixture.path, &info), 0);
    CHECK(S_ISREG(info.st_mode));
    CHECK(info.st_size == 4);

    close(lock_fd);
    bind_dir_remove(&fixture);
}

/* ---- LBD-04: a path too long is rejected ----
 * sun_path holds 108 bytes including the NUL, so 108 characters can't be
 * bound. Nothing is created. */
static void test_lbd04_path_too_long(void)
{
    char long_path[109];
    memset(long_path, 'a', sizeof long_path - 1);
    long_path[0] = '/';
    long_path[sizeof long_path - 1] = '\0';
    int listen_fd = 77;

    CHECK_EQ(gt_listen_bind(long_path, 4, &listen_fd), GT_ERR_INVAL);
    CHECK_EQ(listen_fd, 77);
}

/* ---- LBD-05: after release a restart takes over ----
 * A first copy listens and releases; a second copy (a restart) can then
 * take the lock and bind the same path. */
static void test_lbd05_restart_takes_over(void)
{
    bind_dir fixture;
    CHECK(bind_dir_init(&fixture));
    int lock_fd = take_lock(&fixture);
    int listen_fd = -1;
    CHECK_EQ(gt_listen_bind(fixture.path, 4, &listen_fd), GT_OK);
    gt_listen_release(fixture.path, listen_fd, lock_fd);

    lock_fd = take_lock(&fixture);
    listen_fd = -1;
    CHECK_EQ(gt_listen_bind(fixture.path, 4, &listen_fd), GT_OK);
    CHECK(client_can_connect(fixture.path));

    gt_listen_release(fixture.path, listen_fd, lock_fd);
    bind_dir_remove(&fixture);
}

/* ---- LBD-06: bad arguments are rejected ----
 * NULL args and an empty path; release with -1 descriptors is a no-op. */
static void test_lbd06_bad_args(void)
{
    int listen_fd = 77;

    CHECK_EQ(gt_listen_bind(NULL, 4, &listen_fd), GT_ERR_INVAL);
    CHECK_EQ(gt_listen_bind("", 4, &listen_fd), GT_ERR_INVAL);
    CHECK_EQ(gt_listen_bind("/tmp/x.sock", 4, NULL), GT_ERR_INVAL);
    CHECK_EQ(listen_fd, 77);
    gt_listen_release("/tmp/gt-lbd-never-made.sock", -1, -1);
}

static const test_case_t tests[] = {
    {"LBD-01", "a listener comes up and releases", test_lbd01_listen_and_release},
    {"LBD-02", "a leftover socket file is replaced", test_lbd02_leftover_socket_replaced},
    {"LBD-03", "a non-socket file is left alone", test_lbd03_regular_file_left_alone},
    {"LBD-04", "a path too long is rejected", test_lbd04_path_too_long},
    {"LBD-05", "after release a restart takes over", test_lbd05_restart_takes_over},
    {"LBD-06", "bad arguments are rejected", test_lbd06_bad_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
