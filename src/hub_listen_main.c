/* hub_listen: on-Pi stand-in for the hub process on hub.sock.
 *
 * Purpose: the loopback tests prove the IPC client against a listener in
 * the same test process; this program is a real, separate peer, so the
 * fault tests on the IPC ladder (Sensor-Pipeline.md, rungs 3-5) can run
 * before the hub exists: kill -9 it, kill -STOP it, and feed it from the
 * light and soil processes at once. For each message it prints one line:
 *   light 42 1234                  a DATA message: sensor, counter, raw value
 *   light GAP first=65 count=26    a GAP: counters 65..90 were refused
 *   sensor_seq_gap sensor=light expected=43 got=50
 *                                  counters 43..49 never arrived, uncovered
 * Counters are checked per sensor, and a GAP counts as covering its range,
 * so a correct client never produces a sensor_seq_gap line. A bad packet
 * prints ipc_bad_message and closes that connection, as the DESIGN.md
 * failure-mode table's Hub row says the hub will.
 *
 * Linux model: an AF_UNIX SOCK_SEQPACKET listener, so each recv() returns
 * exactly one message. One thread serves every sensor with poll(), which
 * sleeps until the listener or any connection has something to read.
 * Nothing is stored and no seq or time is stamped: that's gt_hub_accept's
 * job in the real hub. Only one hub_listen can run per socket path: it
 * holds an fcntl lock on "<path>.lock" for its whole life, and only the
 * lock holder may remove and re-create the socket file (see
 * Sensor-Pipeline.md, Decisions; the shared helper is listen_sock.h).
 * A second copy exits with
 * "already running" and touches nothing.
 *
 * Usage: hub_listen /run/plantpi/hub.sock    (Ctrl-C to stop) */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ipc.h"
#include "listen_sock.h"

/* Sensor connections served at once: light and soil, plus room for a
 * restarted sensor whose old connection hasn't been noticed closed yet. */
#define MAX_SENSOR_CONNS 8

/* What the next message from one sensor should carry. Indexed by
 * sensor_id; gt_ipc_frame_decode only lets through LIGHT (1) and SOIL (2). */
typedef struct {
    int      seen;               /* 0 until the first message from this sensor */
    uint32_t expected_counter;
} counter_track;

/* Make sure this is the only hub_listen on `sock_path` (the shared rule in
 * listen_sock.h): hold "<sock_path>.lock" for as long as the process runs.
 * Holding it is what makes it safe for bind_listener to remove an old
 * socket file. Returns the fd that holds the lock (keep it open), or -1
 * after printing why. */
static int take_instance_lock(const char *sock_path)
{
    int lock_fd = -1, already_running = 0;
    gt_status status = gt_listen_lock(sock_path, &lock_fd, &already_running);
    if (status == GT_ERR_INVAL) {
        fprintf(stderr, "hub_listen: socket path must be 1..107 characters\n");
    } else if (status != GT_OK && already_running) {
        fprintf(stderr, "hub_listen: already running on %s (%s.lock is locked)\n",
                sock_path, sock_path);
    } else if (status != GT_OK) {
        fprintf(stderr, "hub_listen: lock %s.lock: %s\n", sock_path, strerror(errno));
    }
    return status == GT_OK ? lock_fd : -1;
}

/* Create the listening socket at `path`, replacing a socket file left by a
 * hub_listen killed with kill -9 (gt_listen_bind). Call only while holding
 * take_instance_lock. The path was already checked by the lock, so
 * GT_ERR_INVAL here means something that is not a socket is in the way.
 * Returns the listening fd, or -1 after printing why. */
static int bind_listener(const char *path)
{
    int listen_fd = -1;
    gt_status status = gt_listen_bind(path, MAX_SENSOR_CONNS, &listen_fd);
    if (status == GT_ERR_INVAL) {
        fprintf(stderr, "hub_listen: %s exists and is not a socket; not removing it\n", path);
    } else if (status != GT_OK) {
        fprintf(stderr, "hub_listen: %s: %s\n", path, strerror(errno));
    }
    return status == GT_OK ? listen_fd : -1;
}

/* Handle one message on a connection that poll() says is readable. It
 * recv()s into a GT_MSG_SIZE + 1 buffer (so an oversized packet shows up
 * as 17, not cut to 16) and decodes it as the hub will. A counter other
 * than the expected one prints sensor_seq_gap. Then the expectation moves
 * past this message: the next counter after DATA, or past the whole
 * refused range after a GAP. `tracks` is the caller's array, one entry
 * per sensor_id. Returns 1 to keep the connection, or 0 when the sensor
 * closed it (recv returned 0) or sent a bad packet; the caller closes it. */
static int serve_one(int conn_fd, counter_track *tracks)
{
    uint8_t buf[GT_MSG_SIZE + 1];
    ssize_t len = recv(conn_fd, buf, sizeof buf, 0);
    gt_msg message;
    gt_status status = gt_ipc_frame_decode(buf, (long)len, &message);
    if (status == GT_ERR_IO) {
        printf("sensor disconnected (fd %d)\n", conn_fd);
        return 0;
    }
    if (status != GT_OK) {
        printf("ipc_bad_message len=%ld (fd %d); closing\n", (long)len, conn_fd);
        return 0;
    }

    const char *sensor_name = message.sensor_id == GT_SENSOR_LIGHT ? "light" : "soil";
    counter_track *track = &tracks[message.sensor_id];
    if (track->seen && message.counter != track->expected_counter) {
        printf("sensor_seq_gap sensor=%s expected=%u got=%u\n", sensor_name,
               (unsigned)track->expected_counter, (unsigned)message.counter);
    }
    if (message.type == GT_MSG_GAP) {
        printf("%s GAP first=%u count=%u\n", sensor_name,
               (unsigned)message.counter, (unsigned)message.value);
        track->expected_counter = message.counter + message.value;
    } else {
        printf("%s %u %u\n", sensor_name, (unsigned)message.counter, (unsigned)message.value);
        track->expected_counter = message.counter + 1;
    }
    track->seen = 1;
    return 1;
}

/* Listen, then serve every sensor from one poll() loop until killed.
 * `struct pollfd` is one entry in poll()'s watch list: `fd` is what to
 * watch, `events` what to wait for (POLLIN: something to read), and the
 * kernel fills in `revents` with what actually happened (POLLIN, or
 * POLLHUP / POLLERR when the peer is gone). poll() sleeps until at least
 * one entry has revents set, so an idle hub_listen uses no CPU.
 * conns[0] is always the listener; conns[1..conn_count-1] are sensors. */
int main(int argc, char **argv)
{
    /* Line-buffer stdout so each message line appears at once, even when
     * the output is piped to a log (e.g. `| tee hub.log`). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc != 2) {
        fprintf(stderr, "usage: %s /run/plantpi/hub.sock\n", argv[0]);
        return 2;
    }
    int lock_fd = take_instance_lock(argv[1]);
    if (lock_fd < 0) {
        return 1;
    }
    int listen_fd = bind_listener(argv[1]);
    if (listen_fd < 0) {
        close(lock_fd);
        return 1;
    }
    printf("hub_listen: listening on %s\n", argv[1]);

    struct pollfd conns[1 + MAX_SENSOR_CONNS];
    conns[0].fd = listen_fd;
    conns[0].events = POLLIN;
    size_t conn_count = 1;
    counter_track tracks[GT_SENSOR_SOIL + 1];
    memset(tracks, 0, sizeof tracks);

    for (;;) {
        if (poll(conns, (nfds_t)conn_count, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("hub_listen: poll");
            break;
        }

        /* Invariant: conns[1..conn_index-1] are open and served this round.
         * A closed connection is replaced by the last entry, which is then
         * checked in the same slot, so no entry is skipped. */
        size_t conn_index = 1;
        while (conn_index < conn_count) {
            if ((conns[conn_index].revents & (POLLIN | POLLHUP | POLLERR)) &&
                !serve_one(conns[conn_index].fd, tracks)) {
                close(conns[conn_index].fd);
                conns[conn_index] = conns[conn_count - 1];
                conn_count--;
                continue;
            }
            conn_index++;
        }

        /* Accept after serving, so a new entry's revents (still 0) is never read. */
        if (conns[0].revents & POLLIN) {
            int conn_fd = accept(listen_fd, NULL, NULL);
            if (conn_fd >= 0 && conn_count < 1 + MAX_SENSOR_CONNS) {
                conns[conn_count].fd = conn_fd;
                conns[conn_count].events = POLLIN;
                conns[conn_count].revents = 0;
                conn_count++;
                printf("sensor connected (fd %d)\n", conn_fd);
            } else if (conn_fd >= 0) {
                printf("too many sensor connections; refusing fd %d\n", conn_fd);
                close(conn_fd);
            }
        }
    }

    /* Invariant: conns[1..conn_index-1] have been closed. */
    for (size_t conn_index = 1; conn_index < conn_count; conn_index++) {
        close(conns[conn_index].fd);
    }
    gt_listen_release(argv[1], listen_fd, lock_fd);   /* the lock goes last */
    return 1;
}
