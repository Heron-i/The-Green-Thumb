#!/usr/bin/env bash
# storage_faults.sh: automated fault rounds against the real storage
# process (DESIGN.md Evaluation plan: "50 random kill -9s on storage: 0
# corrupt records" and "20 SIGTERM restarts: 0 records lost").
#
# One storage_feed -r runs for the whole run, as a hub that keeps running
# while storage restarts: it keeps its own seq counter, so seqs a kill -9
# lost leave a hole that storage must cover with a GAP (PF-03).
#
# Each round:
#   1. start storage_main on a scratch log directory, and wait for its
#      storage_start line (logged once recovery is done);
#   2. check it did not log unclean_shutdown (every start follows a clean stop);
#   3. wait for the feed to reconnect, and check its seq_resume reply equals
#      the last seq the verifier saw on disk after the previous round (no
#      record committed and then lost, none stored twice);
#   4. stop storage at a random moment while the feed sends (kill -9 after
#      1.0-2.9 s, or SIGTERM after 0.1-0.9 s), and wait for the feed to notice;
#   5. kill9 only: pause the feed, start storage once more so it recovers
#      (it must log unclean_shutdown), then SIGTERM it (PF-05);
#   6. run log_verify: it must PASS (every record whole, seq strictly
#      increasing, every hole covered by a GAP), cover no more seqs than
#      the feed sent, and, for sigterm, hold no GAP.
# Then a settle phase: start storage once more so the last hole gets its
# GAP, stop the feed (its last line gives N records sent, seqs A..B), stop
# storage, and check the Evaluation plan's formula: N = B - A + 1 (no seq
# reused), the log spans exactly A..B, and data + gap_readings = N
# (sent - committed - GAP-covered = 0).
# Any failed check is counted and printed; the scratch directory is kept
# for inspection if anything failed, and removed otherwise.
#
# Linux only (WSL or the Pi): storage uses SOCK_SEQPACKET sockets.
#
# Usage: tools/storage_faults.sh kill9|sigterm ROUNDS [BUILD_DIR]
#   e.g. make storage_main storage_feed log_verify
#        tools/storage_faults.sh kill9 50
set -u

MODE="${1:-}"
ROUNDS="${2:-}"
BUILD="${3:-build}"
FEED_RATE=200    # records per second, for the one feed that runs across all rounds

if [[ "$MODE" != "kill9" && "$MODE" != "sigterm" ]] || ! [[ "$ROUNDS" =~ ^[1-9][0-9]*$ ]]; then
    echo "usage: $0 kill9|sigterm ROUNDS [BUILD_DIR]" >&2
    exit 2
fi
for tool in storage_main storage_feed log_verify; do
    if [[ ! -x "$BUILD/$tool" ]]; then
        echo "$0: $BUILD/$tool not found; run: make storage_main storage_feed log_verify" >&2
        exit 2
    fi
done

SIGNAL="-KILL"
[[ "$MODE" == "sigterm" ]] && SIGNAL="-TERM"

WORK="$(mktemp -d /tmp/gt-faults-XXXXXX)"
LOG_DIR="$WORK/log"
SOCK="$WORK/storage.sock"
FEED_OUT="$WORK/feed.out"
failures=0
expected_resume="none"   # what the next seq_resume must say: "none" or a seq
storage_pid=""
feed_pid=""

# Print a failed check and count it.
fail() {
    echo "  FAIL round $round: $*"
    failures=$((failures + 1))
}

# On any exit, kill whatever is still running, so an aborted run doesn't
# leave the feed (which has no record limit) behind.
cleanup() {
    [[ -n "$feed_pid" ]] && kill -KILL "$feed_pid" 2>/dev/null
    [[ -n "$storage_pid" ]] && kill -KILL "$storage_pid" 2>/dev/null
}
trap cleanup EXIT

# Start storage_main, logging to $1, and wait up to 5 s for its
# storage_start line, which it logs once recovery is done. The socket file
# alone can't be trusted: a kill -9 leaves the old one behind. Returns 1
# if the line never appears or storage_main exits first.
start_storage() {
    "$BUILD/storage_main" "$LOG_DIR" "$SOCK" 2>"$1" &
    storage_pid=$!
    for _ in $(seq 1 50); do
        grep -q " storage_start " "$1" && return 0
        kill -0 "$storage_pid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

# Send signal $1 to storage and wait for it to exit. After -TERM, its log
# $2 must hold a clean storage_stop.
stop_storage() {
    kill "$1" "$storage_pid" 2>/dev/null
    wait "$storage_pid" 2>/dev/null
    storage_pid=""
    if [[ "$1" == "-TERM" ]] && ! grep -q " storage_stop .*clean=1" "$2"; then
        fail "no clean storage_stop after SIGTERM (see $2)"
    fi
}

# Wait up to 5 s until the feed's output has at least $2 lines starting
# with $1. Returns 1 on timeout.
wait_feed_lines() {
    for _ in $(seq 1 50); do
        [[ "$(grep -c "^$1" "$FEED_OUT")" -ge "$2" ]] && return 0
        sleep 0.1
    done
    return 1
}

# Print field $1 (e.g. last_seq) of the summary line in $verify_out.
verify_field() {
    echo "$verify_out" | grep -o "$1=[0-9a-z]*" | cut -d= -f2
}

echo "storage_faults: $ROUNDS $MODE rounds in $WORK"
# One feed for the whole run, acting as a hub that keeps running while
# storage restarts (-r): it waits for storage itself and keeps its counter.
"$BUILD/storage_feed" -r "$SOCK" 0 "$FEED_RATE" >"$FEED_OUT" 2>&1 &
feed_pid=$!
rounds_done=0
for round in $(seq 1 "$ROUNDS"); do
    round_log="$WORK/round-$round.log"

    # 1. Start storage.
    if ! start_storage "$round_log"; then
        fail "storage_main did not start (see $round_log)"
        break
    fi

    # 2. Every real start follows a clean stop (a SIGTERM round, or the
    #    recovery pass after a kill -9), so none may report unclean_shutdown.
    if grep -q " unclean_shutdown" "$round_log"; then
        fail "unclean_shutdown logged after a clean stop"
    fi

    # 3. Wait for the feed to reconnect (its seq_resume line for this
    #    round), and check the reply continues exactly after what is on disk.
    if ! wait_feed_lines "seq_resume:" "$round"; then
        fail "no seq_resume line from the feed (see $FEED_OUT)"
        break
    fi
    reply="$(grep "^seq_resume:" "$FEED_OUT" | tail -n 1)"
    if [[ "$expected_resume" == "none" ]]; then
        [[ "$reply" == "seq_resume: empty log"* ]] || fail "expected an empty-log reply, got: $reply"
    else
        [[ "$reply" == "seq_resume: last=$expected_resume,"* ]] \
            || fail "expected seq_resume last=$expected_resume, got: $reply"
    fi

    # 4. Kill storage at a random moment while the feed runs, and wait for
    #    the feed to notice (its "storage gone ..." line for this round).
    #    kill9: 1.0-2.9 s, so kills land anywhere in storage's 1 s flush
    #    cycle (an earlier kill never sees a commit); sigterm: 0.1-0.9 s.
    if [[ "$MODE" == "kill9" ]]; then
        sleep "$((1 + RANDOM % 2)).$((RANDOM % 10))"
    else
        sleep "0.$((RANDOM % 9 + 1))"
    fi
    stop_storage "$SIGNAL" "$round_log"
    if ! wait_feed_lines "storage gone" "$round"; then
        fail "the feed did not notice storage stop (see $FEED_OUT)"
        break
    fi
    gone_seq="$(grep "^storage gone after seq" "$FEED_OUT" | tail -n 1 | grep -o '[0-9]*$')"

    # 5. kill9 only: a recovery pass before verifying (PF-05), so a torn
    #    tail that recovery legally cuts isn't reported. The feed is paused
    #    (SIGSTOP) so it can't connect; storage_start means recovery is done.
    if [[ "$MODE" == "kill9" ]]; then
        recover_log="$WORK/recover-$round.log"
        kill -STOP "$feed_pid"
        if ! start_storage "$recover_log"; then
            fail "recovery storage_main did not start (see $recover_log)"
            break
        fi
        grep -q " unclean_shutdown" "$recover_log" || fail "no unclean_shutdown logged after a kill -9"
        stop_storage -TERM "$recover_log"
    fi

    # 6. Verify the log on disk. The feed started on an empty log, so it
    #    has sent seqs 0..gone_seq: the log can't cover more than that.
    verify_out="$("$BUILD/log_verify" "$LOG_DIR")"
    verify_status=$?
    if [[ "$verify_status" -ne 0 ]]; then
        fail "log_verify: $(echo "$verify_out" | tr '\n' ' ')"
    elif [[ -z "$gone_seq" ]]; then
        fail "the feed sent nothing this round (see $FEED_OUT)"
    else
        held=$(( $(verify_field data) + $(verify_field gap_readings) ))
        (( held <= gone_seq + 1 )) || fail "log covers $held seqs; the feed sent only $((gone_seq + 1))"
        if [[ "$MODE" == "sigterm" && "$(verify_field gap_readings)" -ne 0 ]]; then
            fail "gap_readings=$(verify_field gap_readings) after a clean SIGTERM"
        fi
    fi
    expected_resume="$(verify_field last_seq)"
    echo "round $round: $(echo "$verify_out" | sed -n 2p)"
    [[ "$MODE" == "kill9" ]] && kill -CONT "$feed_pid"
    rounds_done=$round
done

# Settle: start storage once more so the last round's hole gets its GAP,
# stop the feed first (nothing left in flight), then storage, and check
# the Evaluation plan's formula: sent - committed - GAP-covered = 0.
if [[ "$rounds_done" -eq "$ROUNDS" ]]; then
    round="settle"
    settle_log="$WORK/settle.log"
    if ! start_storage "$settle_log"; then
        fail "storage_main did not start (see $settle_log)"
    elif ! wait_feed_lines "seq_resume:" $((ROUNDS + 1)); then
        fail "no seq_resume line from the feed (see $FEED_OUT)"
    else
        sleep 0.5   # the feed sends again, so storage writes the last GAP
        kill -TERM "$feed_pid"
        wait "$feed_pid"
        feed_status=$?
        feed_pid=""
        stop_storage -TERM "$settle_log"
        [[ "$feed_status" -eq 0 ]] || fail "storage_feed exited $feed_status"
        # The feed's final line: "sent N records, seq A..B".
        sent_line="$(grep "^sent " "$FEED_OUT" | tail -n 1)"
        if [[ ! "$sent_line" =~ ^sent\ ([0-9]+)\ records,\ seq\ ([0-9]+)\.\.([0-9]+)$ ]]; then
            fail "no 'sent N records, seq A..B' line from the feed"
        else
            sent_total="${BASH_REMATCH[1]}"
            sent_first="${BASH_REMATCH[2]}"
            sent_last="${BASH_REMATCH[3]}"
            verify_out="$("$BUILD/log_verify" "$LOG_DIR")"
            if [[ $? -ne 0 ]]; then
                fail "log_verify: $(echo "$verify_out" | tr '\n' ' ')"
            else
                held=$(( $(verify_field data) + $(verify_field gap_readings) ))
                (( sent_total == sent_last - sent_first + 1 )) \
                    || fail "the feed reused seqs: $sent_total records in $sent_first..$sent_last"
                [[ "$(verify_field first_seq)" == "$sent_first" ]] \
                    || fail "log starts at $(verify_field first_seq), the feed at $sent_first"
                [[ "$(verify_field last_seq)" == "$sent_last" ]] \
                    || fail "log ends at $(verify_field last_seq), the feed at $sent_last"
                (( held == sent_total )) \
                    || fail "the feed sent $sent_total; committed + GAP-covered is $held"
            fi
            echo "settle: $sent_line; $(echo "$verify_out" | sed -n 2p)"
        fi
    fi
fi

echo "storage_faults: $ROUNDS $MODE rounds, $failures failed checks"
if [[ "$failures" -eq 0 ]]; then
    rm -rf "$WORK"
    exit 0
fi
echo "storage_faults: kept $WORK for inspection"
exit 1
