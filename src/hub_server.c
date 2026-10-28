/* The hub's rx loop (see hub_server.h).
 *
 * Purpose: serve every sensor connection on hub.sock from one poll() loop,
 * turn each message into a record for storage, and keep the hop-2 sender
 * moving, logging every event with its facts (DESIGN.md failure-mode
 * table, Hub rows). The decisions are made elsewhere: gt_ipc_frame_decode
 * checks the frame, gt_hub_accept judges the message, the sender queues
 * and sends. This file only moves bytes between them and reports.
 *
 * Linux model: hub.sock is AF_UNIX SOCK_SEQPACKET, so each recv() is one
 * whole message; reads use MSG_DONTWAIT, so a connection poll() reported
 * is drained without ever blocking. It is a server, so it makes its own
 * socket calls (accept, recv) and is tested over real sockets (PF-16).
 * The time comes from the real clocks: CLOCK_REALTIME stamps records,
 * CLOCK_MONOTONIC drives timeouts and the sender's backoff. */
#include "hub_server.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* One sensor connection. sensor_id is 0 until its first accepted message
 * says which sensor it is. A connection to be closed is marked `doomed`
 * during a pass and removed afterwards, so the poll() list and this table
 * keep the same order while the pass runs. */
typedef struct {
    int      fd;
    uint32_t conn_id;     /* counts up from 1, never reused (fds are) */
    uint8_t  sensor_id;
    int      doomed;
} sensor_conn;

/* Everything one run of the loop keeps. */
typedef struct {
    const gt_hub_serve_ctx *ctx;
    sensor_conn conns[GT_HUB_MAX_SENSOR_CONNS];
    size_t      conn_count;
    uint32_t    next_conn_id;
} serve_state;

/* What reading one connection came to. An enum names a few int values. */
typedef enum {
    CONN_GOT_MESSAGE,   /* one message was handled; there may be more */
    CONN_EMPTY,         /* nothing waiting (EAGAIN) */
    CONN_CLOSE          /* the sensor hung up or sent a bad frame */
} conn_result;

/* Wall-clock time in us since the epoch: what records are stamped with. */
static uint64_t wall_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* Monotonic time in ms: timeouts and backoff, never jumping with NTP. */
static uint64_t mono_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* The name a sensor_id is logged as. */
static const char *sensor_name(uint8_t sensor_id)
{
    if (sensor_id == GT_SENSOR_LIGHT) {
        return "light";
    }
    return sensor_id == GT_SENSOR_SOIL ? "soil" : "unknown";
}

/* The key=value fields for one hub event, so each line carries its facts
 * (expected and got counters, the restart's two times, ...). */
static void hub_event_fields(unsigned flag, const gt_hub_result *result, const gt_msg *message,
                             char *fields, size_t fields_size)
{
    const char *sensor = sensor_name(message->sensor_id);
    unsigned long counter = (unsigned long)message->counter;
    unsigned long expected = (unsigned long)result->expected_counter;
    if (flag == GT_HUB_EV_READING_INVALID) {
        snprintf(fields, fields_size, "sensor=%s counter=%lu value=%lu", sensor, counter,
                 (unsigned long)message->value);
    } else if (flag == GT_HUB_EV_DUPLICATE || flag == GT_HUB_EV_SEQ_GAP) {
        snprintf(fields, fields_size, "sensor=%s expected=%lu got=%lu", sensor, expected, counter);
    } else if (flag == GT_HUB_EV_SENSOR_RESTART) {
        snprintf(fields, fields_size, "sensor=%s last_seen_us=%llu first_us=%llu", sensor,
                 (unsigned long long)result->last_seen_us,
                 (unsigned long long)result->record.timestamp_us);
    } else {
        snprintf(fields, fields_size, "sensor=%s", sensor);
    }
}

/* Log every GT_HUB_EV_* bit in result->events, one line each, lowest bit
 * first. */
static void log_hub_events(const serve_state *state, const gt_hub_result *result,
                           const gt_msg *message)
{
    /* Invariant: every set bit below `flag` has been logged. */
    for (unsigned flag = 1u; flag != 0u && flag <= result->events; flag <<= 1) {
        gt_hub_event event;
        if (!(result->events & flag) || gt_hub_event_describe(flag, &event) != GT_OK) {
            continue;
        }
        char fields[128];
        hub_event_fields(flag, result, message, fields, sizeof fields);
        state->ctx->log(state->ctx->log_ctx, event.level, event.name, fields);
    }
}

/* A sensor's first message on conns[keep_index] just arrived (NEW_CLIENT):
 * any other connection still open for the same sensor is an old instance
 * not yet reaped, so mark it to close and log sensor_duplicate_client
 * (DESIGN.md Hub row: keep the newest). It is closed after this pass. */
static void doom_older_conns(serve_state *state, size_t keep_index)
{
    uint8_t sensor_id = state->conns[keep_index].sensor_id;
    /* Invariant: conns[0..conn_index-1] other than keep_index that belong
     * to this sensor are marked doomed. */
    for (size_t conn_index = 0; conn_index < state->conn_count; conn_index++) {
        sensor_conn *other = &state->conns[conn_index];
        if (conn_index == keep_index || other->doomed || other->sensor_id != sensor_id) {
            continue;
        }
        other->doomed = 1;
        char fields[32];
        snprintf(fields, sizeof fields, "sensor=%s", sensor_name(sensor_id));
        state->ctx->log(state->ctx->log_ctx, GT_LOG_WARN, "sensor_duplicate_client", fields);
    }
}

/* Log every GT_HUB_SENDER_EV_* bit in `events`, one line each, with the
 * sender's facts from its stats: seq_resume says where numbering
 * continued (resume_seq, not next_seq, which the same pump may already
 * have moved past), hub_queue_full how many records have been refused, and a bad
 * reply is marked peer=storage to tell it apart from a sensor's bad frame.
 * The others carry no fields. */
static void log_sender_events(const serve_state *state, unsigned events)
{
    gt_hub_send_stats stats;
    gt_hub_sender_stats(state->ctx->sender, &stats);
    /* Invariant: every set bit below `flag` has been logged. */
    for (unsigned flag = 1u; flag != 0u && flag <= events; flag <<= 1) {
        gt_hub_event event;
        if (!(events & flag) || gt_hub_sender_event_describe(flag, &event) != GT_OK) {
            continue;
        }
        char fields[64] = "";
        if (flag == GT_HUB_SENDER_EV_RESUMED) {
            snprintf(fields, sizeof fields, "next_seq=%lu", (unsigned long)stats.resume_seq);
        } else if (flag == GT_HUB_SENDER_EV_QUEUE_FULL) {
            snprintf(fields, sizeof fields, "refused=%lu", (unsigned long)stats.refused);
        } else if (flag == GT_HUB_SENDER_EV_BAD_REPLY) {
            snprintf(fields, sizeof fields, "peer=storage");
        }
        state->ctx->log(state->ctx->log_ctx, event.level, event.name,
                        fields[0] != '\0' ? fields : NULL);
    }
}

/* Read and handle one message from conns[conn_index], without waiting.
 * The buffer is one byte bigger than a message, so an oversized one shows
 * up as 17 instead of being cut to 16 (SEQPACKET cuts what doesn't fit).
 *   nothing waiting (EAGAIN, or a signal):  CONN_EMPTY
 *   the sensor hung up (recv 0 / error):    sensor_disconnected, CONN_CLOSE
 *   a bad frame (wrong length or fields):   ipc_bad_message, CONN_CLOSE,
 *                                           so the sensor resyncs
 *   a message: gt_hub_accept judges it (its events are logged, and a new
 *   client dooms older connections for the sensor), its record (if any)
 *   is pushed to the sender, and CONN_GOT_MESSAGE is returned. */
static conn_result read_one_message(serve_state *state, size_t conn_index)
{
    sensor_conn *conn = &state->conns[conn_index];
    uint8_t buf[GT_MSG_SIZE + 1];
    ssize_t received = recv(conn->fd, buf, sizeof buf, MSG_DONTWAIT);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return CONN_EMPTY;
    }
    gt_msg message;
    gt_status decoded = gt_ipc_frame_decode(buf, (long)received, &message);
    if (decoded != GT_OK) {
        char fields[48];
        snprintf(fields, sizeof fields, "sensor=%s len=%ld", sensor_name(conn->sensor_id),
                 (long)received);
        int hung_up = decoded == GT_ERR_IO;
        state->ctx->log(state->ctx->log_ctx, hung_up ? GT_LOG_INFO : GT_LOG_WARN,
                        hung_up ? "sensor_disconnected" : "ipc_bad_message", fields);
        return CONN_CLOSE;
    }

    const gt_hub_clock now = { .wall_us = wall_us(), .mono_ms = mono_ms() };
    gt_hub_result result;
    if (gt_hub_accept(state->ctx->hub, conn->conn_id, &message, &now, &result) == GT_ERR_INVAL) {
        return CONN_CLOSE;   /* can't happen: the frame decoded */
    }
    conn->sensor_id = message.sensor_id;
    log_hub_events(state, &result, &message);
    if (result.events & GT_HUB_EV_NEW_CLIENT) {
        doom_older_conns(state, conn_index);
    }
    if (result.has_record) {
        unsigned sender_events = 0;
        gt_hub_sender_push(state->ctx->sender, &result.record, &sender_events);
        log_sender_events(state, sender_events);
    }
    return CONN_GOT_MESSAGE;
}

/* Close and remove every connection marked doomed during the pass. A
 * removed entry is replaced by the last one, which is then checked in the
 * same slot, so none is skipped (the order of the rest doesn't matter
 * between passes: poll()'s list is rebuilt each time). */
static void sweep_doomed(serve_state *state)
{
    size_t conn_index = 0;
    /* Invariant: conns[0..conn_index-1] are open and not doomed. */
    while (conn_index < state->conn_count) {
        if (!state->conns[conn_index].doomed) {
            conn_index++;
            continue;
        }
        close(state->conns[conn_index].fd);
        state->conns[conn_index] = state->conns[state->conn_count - 1];
        state->conn_count--;
    }
}

/* Accept one sensor that is waiting on listen_fd (call only when poll()
 * says one is, so accept() can't block) and give it the next conn_id. Its
 * sensor is unknown until its first message. With the table full the
 * connection is closed at once and logged: the sensor's client sees it
 * close and retries with backoff. */
static void accept_sensor(serve_state *state, int listen_fd)
{
    int conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        return;
    }
    if (state->conn_count == GT_HUB_MAX_SENSOR_CONNS) {
        close(conn_fd);
        state->ctx->log(state->ctx->log_ctx, GT_LOG_WARN, "sensor_refused", "reason=too_many");
        return;
    }
    sensor_conn *conn = &state->conns[state->conn_count++];
    conn->fd = conn_fd;
    conn->conn_id = state->next_conn_id++;
    conn->sensor_id = 0;
    conn->doomed = 0;
}

/* The work every pass does whatever arrived: log each sensor that has just
 * gone quiet (sensor_timeout; the other sensor keeps running), then let
 * the sender connect, read seq_resume or send, and log what it reports. */
static void housekeeping(serve_state *state)
{
    uint64_t now_ms = mono_ms();
    unsigned timed_out = gt_hub_check_timeouts(state->ctx->hub, now_ms);
    gt_hub_event timeout_event;
    gt_hub_event_describe(GT_HUB_EV_TIMEOUT, &timeout_event);
    /* Invariant: every timed-out sensor before sensor_id has been logged. */
    for (uint8_t sensor_id = GT_SENSOR_LIGHT; sensor_id <= GT_SENSOR_SOIL; sensor_id++) {
        if (timed_out & (1u << sensor_id)) {
            char fields[32];
            snprintf(fields, sizeof fields, "sensor=%s", sensor_name(sensor_id));
            state->ctx->log(state->ctx->log_ctx, timeout_event.level, timeout_event.name, fields);
        }
    }
    unsigned sender_events = 0;
    gt_hub_sender_pump(state->ctx->sender, now_ms, &sender_events);
    log_sender_events(state, sender_events);
}

/* On stop, take everything the sensors already handed to the kernel:
 * first accept any sensor still waiting in the listen queue (one that
 * connected just before SIGTERM), then read every connection until it is
 * empty or closes, and pump the sender once more so queued records get a
 * last chance to go out. poll() with a 0 timeout only checks, so nothing
 * here waits. */
static void drain_on_stop(serve_state *state, int listen_fd)
{
    struct pollfd listener = { .fd = listen_fd, .events = POLLIN, .revents = 0 };
    /* Invariant: every sensor accepted so far is in conns[] (or refused). */
    while (poll(&listener, 1, 0) == 1 && (listener.revents & POLLIN)) {
        size_t before = state->conn_count;
        accept_sensor(state, listen_fd);
        if (state->conn_count == before) {
            break;   /* accept failed or the table is full: stop trying */
        }
    }
    /* Invariant: conns[0..conn_index-1] have nothing left to read. */
    for (size_t conn_index = 0; conn_index < state->conn_count; conn_index++) {
        conn_result result = CONN_GOT_MESSAGE;
        while (!state->conns[conn_index].doomed && result == CONN_GOT_MESSAGE) {
            result = read_one_message(state, conn_index);
        }
        if (result == CONN_CLOSE) {
            state->conns[conn_index].doomed = 1;
        }
    }
    sweep_doomed(state);
    housekeeping(state);
}

/* Handle one message from each connection poll() marked readable (or hung
 * up). sensor_watch[i] is poll()'s entry for conns[i]: the table isn't
 * reordered until the sweep at the end, so the two stay lined up. One
 * message per connection per pass keeps a chatty sensor from starving the
 * other; anything left is still readable on the next poll(). */
static void read_ready_sensors(serve_state *state, const struct pollfd *sensor_watch)
{
    /* Invariant: conns[0..conn_index-1] were each served once if ready. */
    for (size_t conn_index = 0; conn_index < state->conn_count; conn_index++) {
        int ready = sensor_watch[conn_index].revents & (POLLIN | POLLHUP | POLLERR);
        if (ready && !state->conns[conn_index].doomed &&
            read_one_message(state, conn_index) == CONN_CLOSE) {
            state->conns[conn_index].doomed = 1;
        }
    }
    sweep_doomed(state);
}

/* The loop (see hub_server.h). poll()'s watch list is rebuilt every pass:
 * watch[0] is the listener, watch[1] the stop pipe, and watch[2 + i] is
 * conns[i]. Its size is a constant, so it's a plain array, not a VLA.
 * poll() returns after GT_HUB_POLL_MS even if nothing happened, so the
 * timeouts and the sender still run. EINTR (a signal arrived) just starts
 * the next pass. The stop pipe is checked before anything else, so a stop
 * is never delayed by a busy sensor. */
gt_status gt_hub_serve(const gt_hub_serve_ctx *ctx, int listen_fd, int stop_fd)
{
    if (ctx == NULL || ctx->hub == NULL || ctx->sender == NULL || ctx->log == NULL ||
        listen_fd < 0 || stop_fd < 0) {
        return GT_ERR_INVAL;
    }
    serve_state state;
    memset(&state, 0, sizeof state);
    state.ctx = ctx;
    state.next_conn_id = 1;

    gt_status status = GT_OK;
    struct pollfd watch[2 + GT_HUB_MAX_SENSOR_CONNS];
    /* Invariant: every message read so far has been judged, logged and
     * (if it made a record) pushed to the sender. */
    for (;;) {
        watch[0] = (struct pollfd){ .fd = listen_fd, .events = POLLIN, .revents = 0 };
        watch[1] = (struct pollfd){ .fd = stop_fd, .events = POLLIN, .revents = 0 };
        for (size_t conn_index = 0; conn_index < state.conn_count; conn_index++) {
            watch[2 + conn_index] = (struct pollfd){ .fd = state.conns[conn_index].fd,
                                                     .events = POLLIN, .revents = 0 };
        }
        int ready = poll(watch, (nfds_t)(2 + state.conn_count), GT_HUB_POLL_MS);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            status = GT_ERR_IO;
            break;
        }
        if (watch[1].revents & POLLIN) {
            drain_on_stop(&state, listen_fd);
            break;
        }
        read_ready_sensors(&state, &watch[2]);
        if (watch[0].revents & POLLIN) {
            accept_sensor(&state, listen_fd);
        }
        housekeeping(&state);
    }

    /* Invariant: conns[0..conn_index-1] have been closed. */
    for (size_t conn_index = 0; conn_index < state.conn_count; conn_index++) {
        close(state.conns[conn_index].fd);
    }
    return status;
}
