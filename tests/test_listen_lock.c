/* Tests for the one-listener lock (src/listen_sock.c): gt_listen_lock.
 *
 * Purpose: prove the rule in Sensor-Pipeline.md's Decisions: a free lock
 * is taken; while another process holds it, a second copy is told it is
 * already running; and when the holder dies, even by kill -9, the kernel
 * frees the lock so a restart can take it. Also shows the fcntl trap the
 * header warns about: inside one process a second take "succeeds", and
 * closing either descriptor frees the lock for everyone.
 *
 * A process never blocks on its own fcntl lock, so the other holder has
 * to be a real second process: each test fork()s a child. A pipe carries
 * one byte from child to parent ("I hold it", or the result of its try),
 * and a second pipe keeps the child alive until the parent closes it. The
 * child always leaves with _exit(), so it never runs this file's test
 * table or the sanitizers' exit checks a second time. Each test uses its
 * own mkdtemp directory under /tmp and removes it after. Run under make
 * test / asan / memcheck. Implements LCK-01 through LCK-05.
 *
 * What these tests cannot prove: a lock on the Pi's real /run/plantpi
 * (tmpfs), which behaves the same but is checked on the IPC ladder. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"
#include "listen_sock.h"

/* Bytes the child sends back to the parent. */
#define CHILD_HOLDS     'h'   /* took the lock and is holding it */
#define CHILD_GOT_IT    'o'   /* a try returned GT_OK */
#define CHILD_BUSY      'r'   /* a try said another process holds it */
#define CHILD_FAILED    'e'   /* anything else */

/* One test's directory and the socket path in it (no socket is made:
 * only "<path>.lock" is). */
typedef struct {
    char dir[32];    /* "/tmp/gt-lck-XXXXXX", the X's filled in by mkdtemp */
    char path[64];   /* dir + "/test.sock" */
} lock_dir;

/* Make a fresh directory. Returns 1 on success, 0 on failure. */
static int lock_dir_init(lock_dir *fixture)
{
    strcpy(fixture->dir, "/tmp/gt-lck-XXXXXX");
    if (mkdtemp(fixture->dir) == NULL) {
        return 0;
    }
    snprintf(fixture->path, sizeof fixture->path, "%s/test.sock", fixture->dir);
    return 1;
}

/* Remove the lock file and the directory. */
static void lock_dir_remove(const lock_dir *fixture)
{
    char lock_path[80];
    snprintf(lock_path, sizeof lock_path, "%s.lock", fixture->path);
    unlink(lock_path);
    rmdir(fixture->dir);
}

/* In the child: take the lock, report CHILD_HOLDS (or CHILD_FAILED), then
 * wait until the parent closes keep_alive_read (read returns 0) or kills
 * this process. Never returns. */
static void child_hold_lock(const char *path, int report_write, int keep_alive_read)
{
    int lock_fd = -1, already_running = 0;
    char report = gt_listen_lock(path, &lock_fd, &already_running) == GT_OK ? CHILD_HOLDS
                                                                           : CHILD_FAILED;
    char ignored;
    if (write(report_write, &report, 1) != 1) {
        _exit(1);
    }
    /* Blocks until the parent closes its end of the pipe, or kills us. */
    while (read(keep_alive_read, &ignored, 1) > 0) {
    }
    _exit(0);
}

/* In the child: try the lock once and report the result. Never returns. */
static void child_try_lock(const char *path, int report_write)
{
    int lock_fd = -1, already_running = 0;
    gt_status status = gt_listen_lock(path, &lock_fd, &already_running);
    char report = status == GT_OK ? CHILD_GOT_IT
                : (status == GT_ERR_IO && already_running) ? CHILD_BUSY
                : CHILD_FAILED;
    if (write(report_write, &report, 1) != 1) {
        _exit(1);
    }
    _exit(0);
}

/* A running child that holds the lock: its pid, the pipe end the parent
 * closes to let it go, and its first report. */
typedef struct {
    pid_t pid;
    int   keep_alive_write;
    char  report;
} holder;

/* Fork a child that takes the lock and holds it. Returns 1 once the child
 * has reported (holder->report says what it got), 0 if fork or a pipe
 * failed. pipe() fills two descriptors: [0] to read, [1] to write. */
static int start_holder(const char *path, holder *child)
{
    int report_pipe[2], keep_alive_pipe[2];
    if (pipe(report_pipe) != 0) {
        return 0;
    }
    if (pipe(keep_alive_pipe) != 0) {
        close(report_pipe[0]);
        close(report_pipe[1]);
        return 0;
    }
    child->pid = fork();
    if (child->pid == 0) {
        close(report_pipe[0]);
        close(keep_alive_pipe[1]);
        child_hold_lock(path, report_pipe[1], keep_alive_pipe[0]);
    }
    close(report_pipe[1]);
    close(keep_alive_pipe[0]);
    child->keep_alive_write = keep_alive_pipe[1];
    child->report = CHILD_FAILED;
    int ok = child->pid > 0 && read(report_pipe[0], &child->report, 1) == 1;
    close(report_pipe[0]);
    return ok;
}

/* Let the holder go (close the keep-alive pipe) and wait for it to exit. */
static void stop_holder(holder *child)
{
    close(child->keep_alive_write);
    if (child->pid > 0) {
        waitpid(child->pid, NULL, 0);
    }
}

/* Fork a child that tries the lock once; return its report byte. */
static char try_in_child(const char *path)
{
    int report_pipe[2];
    char report = CHILD_FAILED;
    if (pipe(report_pipe) != 0) {
        return report;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(report_pipe[0]);
        child_try_lock(path, report_pipe[1]);
    }
    close(report_pipe[1]);
    if (pid > 0 && read(report_pipe[0], &report, 1) != 1) {
        report = CHILD_FAILED;
    }
    close(report_pipe[0]);
    if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
    return report;
}

/* ---- LCK-01: a free lock is taken ---- */
static void test_lck01_free_lock_taken(void)
{
    lock_dir fixture;
    CHECK(lock_dir_init(&fixture));
    int lock_fd = -1, already_running = 7;

    CHECK_EQ(gt_listen_lock(fixture.path, &lock_fd, &already_running), GT_OK);
    CHECK(lock_fd >= 0);
    CHECK_EQ(already_running, 0);

    close(lock_fd);
    lock_dir_remove(&fixture);
}

/* ---- LCK-02: a held lock says already running ----
 * While a child holds it, the parent's try fails with already_running = 1
 * and lock_fd untouched. */
static void test_lck02_held_lock_already_running(void)
{
    lock_dir fixture;
    CHECK(lock_dir_init(&fixture));
    holder child;
    int lock_fd = 77, already_running = 0;

    CHECK(start_holder(fixture.path, &child));
    CHECK_EQ(child.report, CHILD_HOLDS);
    CHECK_EQ(gt_listen_lock(fixture.path, &lock_fd, &already_running), GT_ERR_IO);
    CHECK_EQ(already_running, 1);
    CHECK_EQ(lock_fd, 77);

    stop_holder(&child);
    lock_dir_remove(&fixture);
}

/* ---- LCK-03: kill -9 frees the lock ----
 * The holder is killed with SIGKILL, so it runs no clean-up at all. Once
 * it is reaped, the kernel has dropped its lock and the parent takes it. */
static void test_lck03_kill9_frees_lock(void)
{
    lock_dir fixture;
    CHECK(lock_dir_init(&fixture));
    holder child;
    int lock_fd = -1, already_running = 0;

    CHECK(start_holder(fixture.path, &child));
    CHECK_EQ(child.report, CHILD_HOLDS);
    CHECK_EQ(kill(child.pid, SIGKILL), 0);
    stop_holder(&child);

    CHECK_EQ(gt_listen_lock(fixture.path, &lock_fd, &already_running), GT_OK);
    CHECK_EQ(already_running, 0);

    close(lock_fd);
    lock_dir_remove(&fixture);
}

/* ---- LCK-04: the one-process trap ----
 * A second take in the same process succeeds (fcntl locks belong to the
 * process). Closing only the second descriptor then frees the lock for
 * everyone: a child can take it although the first descriptor is still
 * open. This is why a process must take the lock exactly once. */
static void test_lck04_one_process_trap(void)
{
    lock_dir fixture;
    CHECK(lock_dir_init(&fixture));
    int first_fd = -1, second_fd = -1, already_running = 0;

    CHECK_EQ(gt_listen_lock(fixture.path, &first_fd, &already_running), GT_OK);
    CHECK_EQ(try_in_child(fixture.path), CHILD_BUSY);
    CHECK_EQ(gt_listen_lock(fixture.path, &second_fd, &already_running), GT_OK);
    close(second_fd);
    CHECK_EQ(try_in_child(fixture.path), CHILD_GOT_IT);

    close(first_fd);
    lock_dir_remove(&fixture);
}

/* ---- LCK-05: bad arguments are rejected ----
 * NULL args, an empty path and one too long for a socket address. */
static void test_lck05_bad_args(void)
{
    char long_path[160];
    memset(long_path, 'a', sizeof long_path - 1);
    long_path[0] = '/';
    long_path[sizeof long_path - 1] = '\0';
    int lock_fd = -1, already_running = 0;

    CHECK_EQ(gt_listen_lock(NULL, &lock_fd, &already_running), GT_ERR_INVAL);
    CHECK_EQ(gt_listen_lock("/tmp/x.sock", NULL, &already_running), GT_ERR_INVAL);
    CHECK_EQ(gt_listen_lock("/tmp/x.sock", &lock_fd, NULL), GT_ERR_INVAL);
    CHECK_EQ(gt_listen_lock("", &lock_fd, &already_running), GT_ERR_INVAL);
    CHECK_EQ(gt_listen_lock(long_path, &lock_fd, &already_running), GT_ERR_INVAL);
    CHECK_EQ(lock_fd, -1);
}

static const test_case_t tests[] = {
    {"LCK-01", "a free lock is taken", test_lck01_free_lock_taken},
    {"LCK-02", "a held lock says already running", test_lck02_held_lock_already_running},
    {"LCK-03", "kill -9 frees the lock", test_lck03_kill9_frees_lock},
    {"LCK-04", "the one-process trap", test_lck04_one_process_trap},
    {"LCK-05", "bad arguments are rejected", test_lck05_bad_args},
};

int main(void)
{
    return RUN_TESTS(tests);
}
