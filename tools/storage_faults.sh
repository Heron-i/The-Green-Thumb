#!/usr/bin/env bash
# storage_faults.sh: automated fault rounds against the real storage
# process (DESIGN.md Evaluation plan: "50 random kill -9s on storage: 0
# corrupt records" and "20 SIGTERM restarts: 0 records lost").
#
# Each round:
#   1. start storage_main on a scratch log directory, wait for its socket;
#   2. check what it logged at start: after a kill -9 round it must report
#      unclean_shutdown; after a SIGTERM round it must not;
#   3. run storage_feed, and check its seq_resume reply equals the last
#      seq the verifier saw on disk after the previous round (no record
#      committed and then lost, none stored twice);
#   4. kill storage at a random moment while the feed runs (kill -9 or
#      SIGTERM), and wait for it to exit;
#   5. run log_verify on the log: it must PASS (every record whole, seq
#      strictly increasing, every hole covered by a GAP).
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
FEED_COUNT=300   # records per round
FEED_RATE=200    # records per second: about 1.5 s of feeding per round

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
failures=0
expected_resume="none"   # what the next seq_resume must say: "none" or a seq

# Print a failed check and count it.
fail() {
    echo "  FAIL round $round: $*"
    failures=$((failures + 1))
}

# Wait up to 5 s for storage_main to create its socket. Returns 0 once it
# is there, 1 if it never appears or storage_main exits first.
wait_for_socket() {
    for _ in $(seq 1 50); do
        [[ -S "$SOCK" ]] && return 0
        kill -0 "$storage_pid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

echo "storage_faults: $ROUNDS $MODE rounds in $WORK"
for round in $(seq 1 "$ROUNDS"); do
    round_log="$WORK/round-$round.log"

    # 1. Start storage.
    "$BUILD/storage_main" "$LOG_DIR" "$SOCK" 2>"$round_log" &
    storage_pid=$!
    if ! wait_for_socket; then
        fail "storage_main did not start (see $round_log)"
        kill -KILL "$storage_pid" 2>/dev/null
        wait "$storage_pid" 2>/dev/null
        break
    fi

    # 2. What it said at start about the last run.
    if [[ "$round" -gt 1 ]]; then
        if [[ "$MODE" == "kill9" ]] && ! grep -q " unclean_shutdown" "$round_log"; then
            fail "no unclean_shutdown logged after a kill -9"
        fi
        if [[ "$MODE" == "sigterm" ]] && grep -q " unclean_shutdown" "$round_log"; then
            fail "unclean_shutdown logged after a clean SIGTERM"
        fi
    fi

    # 3. Feed, and check the reply continues exactly after what is on disk.
    "$BUILD/storage_feed" "$SOCK" "$FEED_COUNT" "$FEED_RATE" >"$WORK/feed-$round.out" 2>&1 &
    feed_pid=$!
    sleep 0.2   # long enough for the feed to have read its reply
    reply="$(head -n 1 "$WORK/feed-$round.out")"
    if [[ "$expected_resume" == "none" ]]; then
        [[ "$reply" == "seq_resume: empty log"* ]] || fail "expected an empty-log reply, got: $reply"
    else
        [[ "$reply" == "seq_resume: last=$expected_resume,"* ]] \
            || fail "expected seq_resume last=$expected_resume, got: $reply"
    fi

    # 4. Kill storage at a random moment while the feed runs.
    sleep "0.$((RANDOM % 9 + 1))"
    kill "$SIGNAL" "$storage_pid" 2>/dev/null
    wait "$storage_pid" 2>/dev/null
    wait "$feed_pid" 2>/dev/null   # it fails once storage is gone; that's expected

    if [[ "$MODE" == "sigterm" ]] && ! grep -q " storage_stop .*clean=1" "$round_log"; then
        fail "no clean storage_stop after SIGTERM"
    fi

    # 5. Verify the log on disk.
    verify_out="$("$BUILD/log_verify" "$LOG_DIR")"
    verify_status=$?
    if [[ "$verify_status" -ne 0 ]]; then
        fail "log_verify: $(echo "$verify_out" | tr '\n' ' ')"
    fi
    expected_resume="$(echo "$verify_out" | grep -o 'last_seq=[0-9a-z]*' | cut -d= -f2)"
    echo "round $round: $(echo "$verify_out" | sed -n 2p)"
done

echo "storage_faults: $ROUNDS $MODE rounds, $failures failed checks"
if [[ "$failures" -eq 0 ]]; then
    rm -rf "$WORK"
    exit 0
fi
echo "storage_faults: kept $WORK for inspection"
exit 1
