# Pipeline Findings

Adversarial review of branch `Menu-Mechanism-D` (Lucas's Mechanism D storage work) against `DESIGN.md` and `Sensor-Pipeline.md`, before it merges to `main`. Reviewed 2026-10-26 at commit `7741f69`, covering every change since the `main` merge `d9344f3`.

**How it was checked:** every new or changed file in `src/`, plus `tools/storage_faults.sh` and the `DESIGN.md` / `Sensor-Pipeline.md` edits, was read line by line. No code was changed. `storage_main`, `storage_feed` and `log_verify` were built into a scratch directory, and `tools/storage_faults.sh sigterm 2` was run once in WSL (see PF-04).

**Labels:**
- **[Both docs]:** contradicts `Sensor-Pipeline.md` *and* `DESIGN.md`.
- **[Breaks plan]:** a change that stops something `Sensor-Pipeline.md` intends from working as planned.
- **Confirmed:** shown by reading the code and, where noted, by running it. **Plausible:** follows from the code but not seen happening.

## Summary

| ID | Finding | Contradicts | Severity | Status |
|---|---|---|---|---|
| PF-01 | The hub can't stamp `seq` when a message arrives | [Both docs] [Breaks plan] | High | Decided in docs; hub not built |
| PF-02 | The hop-2 sender can't reuse the `ipc_sock` seam | Sensor-Pipeline.md [Breaks plan] | High | Decided in docs; seam not changed |
| PF-03 | A storage `kill -9` loses records with no trace in the log | [Both docs] | High | Decided in docs; `gt_next_seq`, `storage_feed` and the fault rounds follow it; hub sender not built |
| PF-04 | `storage_faults.sh` fails every round | Tooling (confirmed by a run) | High | Resolved (2026-10-26) |
| PF-05 | `kill9` rounds verify the log before recovery has run | Tooling (plausible) | Medium | Resolved (2026-10-26) |
| PF-06 | Records from the end of a day go into the next day's segment | DESIGN.md | Medium | Resolved (2026-10-26) |
| PF-07 | A storage-made GAP record carries the gap's end time | DESIGN.md | Medium | Resolved (2026-10-26) |
| PF-08 | Commits are not logged and fsync is not timed | DESIGN.md (2 rows) | Medium | Open |
| PF-09 | DESIGN.md describes disk-full and read-only handling as done | DESIGN.md | Medium | Resolved in docs (2026-10-26); handling not built |
| PF-10 | Storage heartbeat: not built, and DESIGN.md disagrees with itself | DESIGN.md | Medium | Open |
| PF-11 | The (N, T) batching experiment can't run as specified | DESIGN.md | Low | Open |
| PF-12 | The verifier doesn't compare against the hub's sent `seq`s | DESIGN.md | Low | Open |
| PF-13 | `clock_unsynced` / `clock_synced` are never logged | DESIGN.md | Low | Open |
| PF-14 | Some log lines are missing their `key=value` fields | DESIGN.md | Low | Open |
| PF-15 | Doc errors: "lux" range check, wrong decode function | [Both docs] / DESIGN.md | Low | Resolved (2026-10-26) |
| PF-16 | Socket calls outside `src/ipc_sock.c` | Sensor-Pipeline.md | Low | Open |
| PF-17 | Lily's Status section edited; "storage is done" overclaims | Sensor-Pipeline.md | Low | Open |
| PF-18 | The log sink was chosen by one side | Sensor-Pipeline.md | Low | Open |
| PF-19 | The Code map is out of date | Sensor-Pipeline.md | Low | Open |
| PF-20 | Smaller storage edge cases | DESIGN.md | Low | Open |

Resolved since the first pass: `storage_main.c` now exists, `seq_resume` is documented in DESIGN.md, and DESIGN.md's memory budget now matches the single pending buffer (see *What meets the plans*).

**Progress (2026-10-26):** PF-01 to PF-03 are settled on paper in both documents. Each finding below has a **Resolution** line saying what was decided and what is still to do. Later the same day, PF-04 and PF-05 were fixed in code, PF-03's rule was built as `gt_next_seq` and used by `storage_feed`, and `sigterm 20` and `kill9 50` were run with 0 failed checks. Storage itself was not changed.

## High: the `seq_resume` hand-off and the hub

These three come from the same change: on every connect, storage now sends the hub an 8 B `seq_resume` reply, and DESIGN.md says "the hub continues from the next value". They decide the hub's design, so they should be settled in the PR before the hub is written.

### PF-01: The hub can't stamp `seq` when a message arrives [Both docs] [Breaks plan]
- **Sensor-Pipeline.md** (*Decisions*, "Hop 2 … carries the 24 B on-disk record", 2026-10-23): the hub stamps `seq` and the µs time and builds the whole record, CRC included, as each message arrives; the record then waits in the 1024-record queue. `include/hub.h` matches: `gt_hub_accept(h, msg, now_us, gt_record *out)` returns a finished record.
- **DESIGN.md:** the hub stamps `seq`; the hub queue is "1024 records × 24 B"; Mechanism E requires that processes "tolerate unavailable peers during startup".
- **What breaks:** a hub that has just started doesn't know its `seq` until storage replies. If storage is down or still recovering when the hub starts, which Mechanism E says must be tolerated, `gt_hub_accept` can't fill `seq` and so can't compute the CRC. The hub must either stop taking sensor messages (breaking Mechanism E) or queue records without a `seq` and stamp `seq` + CRC at send time, which changes `hub.h`, what the queue holds, and DESIGN.md's "records" wording.
- **Still possible:** the timestamp can still be taken on arrival, so a queued record keeps its real receive time.
- **Resolution (decided 2026-10-26, docs only):**
  - **The rule:** the hub stamps the time on arrival, and `seq` + CRC just before `send()`. The 1024-record queue holds records without `seq` or CRC, and a `seq` is used only when `send()` returns OK.
  - **Where it's written:**
    - Sensor-Pipeline.md: the new *Hub contract (hop 2)* section, a Decisions bullet, an amendment note on the 2026-10-23 hop-2 Decision, and the *Later* bullets.
    - DESIGN.md: both Hub → storage rows, the hub-queue budget row, and *At what rate*.
  - **Still to do:** rewrite the `gt_hub_accept` comment in `include/hub.h` when the hub steps start.

### PF-02: The hop-2 sender can't reuse the `ipc_sock` seam [Breaks plan]
- **Sensor-Pipeline.md** (*Later*): the sender "reuses the `ipc_sock` seam to send 24 B records".
- **The seam can't receive:** `gt_sock_ops` has only `connect`, `send`, `disconnect` and `destroy` ([include/ipc_sock.h:19-32](include/ipc_sock.h#L19-L32)).
- **What `seq_resume` needs:** a `recv` op; a third state between connecting and sending ("connected, waiting for the reply"), where the plan's state machine (*IPC client: connection and ring rules*) has only DISCONNECTED and CONNECTED; and a rule for a reply that never comes, without blocking (`storage_feed` simply waits up to 2 s). `tests/fake_sock.h` would need the same additions.
- **DESIGN.md** documents the reply, so it is consistent here; Sensor-Pipeline.md is not.
- **Resolution (decided 2026-10-26, docs only):**
  - **The seam:** `gt_sock_ops` gains a non-blocking `recv` op. It returns 0 bytes when nothing is waiting and `GT_ERR_IO` when the peer has closed.
  - **The sender:** it gets an `AWAITING_RESUME` state between DISCONNECTED and SENDING, and sends nothing until the reply arrives. A reply that doesn't come within 2 s, or a bad one, means disconnect and retry with backoff.
  - **Where it's written:** in Sensor-Pipeline.md's *Hub contract (hop 2)*: the state diagram, the API sketch (`hub_sender.h`), the `fake_sock.h` additions and the reserved test prefixes (`HSO`, `HSR`, `HSS`, `HSQ`, `HSL`).
  - **Still to do:** the header, fake and sender, test first.

### PF-03: A storage `kill -9` loses records with no trace in the log [Both docs]
- **Sensor-Pipeline.md** treats every loss as visible: "whichever component refuses a reading records the gap"; a message lost in transit shows up as "the jump in the counter" (`sensor_seq_gap`); ladder rung 4 requires the data counters plus `gap.value` to "cover every counter".
- **DESIGN.md:** the row "Storage killed (`kill -9` or OOM) with an unsynced batch" promises one GAP record covering the lost range (`recovery_gap`), and the Evaluation plan counts lost readings as "seqs the hub sent − seqs committed − seqs covered by a GAP record".
- **What breaks:** this PR changed the hand-off to "storage replies with its last **accepted** `seq` … and the hub continues from the next value". After a `kill -9`, the accepted-but-unsynced records are gone, so the reply is the last `seq` on disk. A hub that continues from there reuses the lost records' `seq`s: no hole appears, no GAP is written, and the hub's sent list holds the same `seq` twice, so the Evaluation plan's formula no longer works.
- **The code disagrees with itself:** the header comment in [src/storage_feed_main.c:11-12](src/storage_feed_main.c#L11-L12) says the next records "start a recovery_gap", but the code sends from `resume + 1` ([src/storage_feed_main.c:73](src/storage_feed_main.c#L73)), so it never does. `storage_faults.sh`'s check that "the reply equals the last `seq` on disk" passes precisely because the loss isn't marked.
- **Proposed fix (a DESIGN.md decision):** a hub that is still running keeps its own counter, so storage sees the hole and writes the GAP; only a freshly started hub uses `resume + 1`. In short, the next `seq` is the larger of the hub's own next `seq` and `resume + 1`.
- **Resolution (decided 2026-10-26, docs only): the proposed fix was adopted.**
  - **The rule:** next `seq` = max(own next, `resume + 1`), where the hub's own counter is 1 + the last `seq` whose `send()` succeeded.
  - **Storage needs no change:** `gt_seq_check` already returns GAP for a hole, and `recovery_gap` is logged.
  - **DESIGN.md:**
    - The hub row states the max rule.
    - The Storage-killed row no longer says "no GAP record". It names the cases with no GAP: the hub restarted too (shown by `unclean_shutdown` only), or the log was empty.
    - The `has_last` field says only a fresh hub starts at 0.
    - The Evaluation plan's lost-readings formula is marked exact within one hub run. Power pulls use the "loss ≤ 1 batch" target instead.
  - **Sensor-Pipeline.md:** a new Decision (with the rejected alternative), plus the Mechanism D *Not done yet* bullet.
  - **The rule is shared:** a pure `gt_next_seq` in `storage_proto.h`, called by both the hub's sender and `storage_feed`, so the fault rounds test the hub's own numbering code.
  - **Done (with PF-04/05, 2026-10-26):**
    - `gt_next_seq` written test first (NSQ 8/8, red then green).
    - `storage_feed` uses it, its header comment is fixed, and its new `-r` mode keeps its counter across storage restarts.
    - `storage_faults.sh` runs one feed across all rounds. In `kill9 50`, each of the 50 kills left exactly one GAP, written at the next start (round *n* showed `gaps=n−1`, and the settle start added the 50th; 5493 readings covered). The final check found sent = committed + GAP-covered.
  - **Still to do:** the hub's hop-2 sender calls `gt_next_seq` (PF-02's sender steps).

## Tooling bugs

### PF-04: `storage_faults.sh` fails every round (confirmed by a run)
A run of `tools/storage_faults.sh sigterm 2` in WSL:

```
FAIL round 1: expected an empty-log reply, got:
FAIL round 2: expected seq_resume last=59, got:
storage_faults: 2 sigterm rounds, 2 failed checks
```

Storage itself was correct: the feed's output file later held `seq_resume: last=59, sending from 60`, and the round log showed `storage_stop clean=1` with no `unclean_shutdown`.
- **Cause:** `storage_feed` prints with `printf`, and stdout redirected to a file is fully buffered until the program exits. The script reads the file after `sleep 0.2` ([tools/storage_faults.sh:97-98](tools/storage_faults.sh#L97-L98)), before the line has been written.
- **Fix:** `setvbuf(stdout, NULL, _IOLBF, 0)` (line buffering) or an `fflush(stdout)` after the reply line in `storage_feed`, then re-run `sigterm 20` and `kill9 50`. The PR's status table marks these runs "Not recorded", which is accurate.
- **Resolution (2026-10-26):**
  - `storage_feed` sets line buffering first thing in `main`. A smoke run showed the reply line in the output file while the feed was still running.
  - The script no longer sleeps a fixed time. It waits up to 5 s for the round's `seq_resume:` line.
  - **Also found while fixing it:** a `kill -9` leaves the old socket file behind, so the old "wait for the socket" step returned before the new storage had recovered. The script now waits for storage's `storage_start` log line.
  - **Results:** `sigterm 20` gave 0 failed checks: 1773 sent, 1773 committed, 0 GAP readings. `kill9 50` is under PF-05.

### PF-05: `kill9` rounds verify the log before recovery has run (plausible)
- `log_verify` runs right after the kill, before the next start's recovery ([tools/storage_faults.sh:116-117](tools/storage_faults.sh#L116-L117)).
- A SIGKILL can cut a large `pwrite` short, leaving a partial tail. DESIGN.md allows that, because recovery truncates it, and its target is "0 corrupt records accepted **after restart**". The verifier would still report `partial_tail` as a failure.
- **Fix:** in `kill9` mode, start and stop storage once (so recovery runs) before verifying, or verify at the start of the next round.
- **Resolution (2026-10-26):**
  - **The recovery pass:** each `kill9` round now pauses the feed, starts storage once so it recovers (it must log `unclean_shutdown`), SIGTERMs it (it must log `storage_stop clean=1`), and only then runs `log_verify`.
  - **Also found while fixing it:** the old 0.1–0.9 s kill delay always landed before storage's 1 s flusher tick. A trial run showed `data=0` in every round with 0 failed checks: nothing was ever committed, every loss hit the empty-log case (no GAP), and torn-tail recovery was never exercised. The `kill9` delay is now a random 1.0–2.9 s, so kills land anywhere in the flush cycle.
  - **Results:** `kill9 50` gave 0 failed checks and `corrupt_files=0` in every round: 18742 sent = 13249 committed + 5493 GAP-covered.
  - **Not shown:** whether any of the 50 recoveries actually cut a torn tail (`records_removed > 0`). The script deletes its work directory after a passing run, so that evidence wasn't kept.

## Contradicts DESIGN.md (storage)

### PF-06: Records from the end of a day go into the next day's segment
- **DESIGN.md:** "one segment file per UTC day".
- `gt_flusher_tick` rolls over to the new day's segment *before* it flushes ([src/storage_server.c:212-228](src/storage_server.c#L212-L228)), and `gt_storage_rollover` doesn't flush first. Up to about 60 s of records received before midnight are written into the new day's file.
- **Fix:** flush (or attempt to) before switching segments.
- **Resolution (2026-10-26):** the fix was adopted.
  - **The change:** `gt_storage_rollover` flushes pending records into the old segment before creating the new one, and its events are passed on. If that flush fails, the existing degraded path cuts the old tail and the records go into the new segment.
  - **Remaining limit:** records that arrive between midnight and the flusher's next pass (≤ 1 s) still go into the old day's file. This is now written in DESIGN.md.
  - **Tests:** SRO-13 and SRO-14 are new. SRO-11, SRO-12 and FTK-05 were changed with approval, because they asserted the old behaviour; each kept its scenario. All were seen red, then green.

### PF-07: A storage-made GAP record carries the gap's end time
- **DESIGN.md** record table: a GAP's timestamp is the "gap start"; "a gap ends at the timestamp of the next DATA record".
- `gt_seq_gap_record` uses the incoming record's timestamp ([src/storage_seq.c:42](src/storage_seq.c#L42)), so start and end come out identical and the gap's length is lost. Its `seq` (the first refused reading) and `value` are correct.
- **Resolution (2026-10-26):**
  - **The rule:** a storage-made GAP is stamped with the last accepted record's time, the earliest the gap could have started.
  - **After a restart:** recovery reports the newest record's time (`last_timestamp_us` in `gt_scan_result`, `gt_recover_report` and `gt_storage_open_info`), so a GAP found after a `kill -9` gets it too.
  - **Signature change:** `gt_seq_gap_record` gained a `last_accepted_us` argument.
  - **Tests:** RSC-10, SRV-11, SOP-13 and SFL-11 are new, and SBL-03 gained a timestamp check. SQG-07 and SFL-03 were changed with approval, because they asserted the end time. All were seen red, then green (322 tests).
  - **Real process:** in a manual `kill -9` run, the GAP for seqs 372–458 carried record 371's time, and the next record showed it lasted 1.063 s. `storage_faults.sh kill9 5` and `sigterm 3` still gave 0 failed checks.

### PF-08: Commits are not logged and fsync is not timed
- **DESIGN.md**, Mechanism D: "Every commit logged with count and duration; report fsync p50/p99". The `fsync_slow` row: "fsync duration measured on every commit; log any fsync over 1 s".
- `gt_storage_flush` reports no event on success and doesn't time the fsync, so neither row can be measured.

### PF-09: DESIGN.md describes disk-full and read-only handling as done
- The `storage_disk_full` row (`ENOSPC`, `statvfs`, the 64 KB reserve) and the `storage_readonly` row (`EROFS`) read as built. Nothing in `src/` mentions `statvfs`, `ENOSPC` or `EROFS`; a full card shows up only as `storage_write_error` + `storage_degraded`.
- Lucas's *Not done yet (Mechanism D)* list says so. DESIGN.md should mark both rows as planned until they land.
- **Resolution (2026-10-26, docs only):**
  - **Disk-full row:** ENOSPC / `statvfs` and `storage_disk_full` are marked planned. The built parts (backlog at 50/75/90%, refuse and count when the buffer is full, the GAP on recovery) are left as they were.
  - **Read-only row:** marked `Storage (planned)`, matching the *Analysis (planned)* rows.
  - **Today's behaviour is stated in both rows:** a full card or EROFS shows as `storage_write_error` (or `fsync_error`, or `segment_create_error` at rollover) + `storage_degraded`, with the records kept in memory.
  - ***With what retention*:** the disk-full steps and the 64 KB reserve are marked planned too.
  - **Still to do:** the handling itself, which needs a "disk full" status out of the file seam first (Lucas's *Not done yet*).

### PF-10: Storage heartbeat: not built, and DESIGN.md disagrees with itself
- The architecture diagram has the **flush thread** send a 1 Hz heartbeat to `sup.sock`; the `fsync_slow` row says the **rx thread** sends it (so a stalled fsync can't stop it).
- Neither is built; `storage_main.c` says it "arrives with Mechanism E". DESIGN.md needs one answer before it is.

### PF-11: The (N, T) batching experiment can't run as specified
- **DESIGN.md**, Mechanism D: compare fsync count, latency and CPU at (N, T) = (1, 1 s), (64, 10 s) and (64, 60 s).
- N and T are compile-time `#define`s, and the flush thread checks only once a second ([src/storage_server.c:26](src/storage_server.c#L26)), so N = 1 can't mean one fsync per record, and the N = 64 trigger can be up to 1 s late.

### PF-12: The verifier doesn't compare against the hub's sent `seq`s
- **DESIGN.md**, Test tooling: the verifier "then compares the log against the hub's sent-sequence log".
- `log_verify` checks every CRC, that `seq` strictly increases across segments, and that every hole is covered by a GAP, but there is no hub yet to compare against. Together with PF-03, a `kill -9` loss is currently undetectable.

### PF-13: `clock_unsynced` / `clock_synced` are never logged
- **DESIGN.md**, System clock row: log the unsynced period with `clock_unsynced` / `clock_synced`.
- `storage_main`'s `clock_synced()` is sound (a 2026-10-01 floor plus `adjtimex`, which returned TIME_OK in WSL), but only `retention_skipped` is logged, once per unsynced spell.

### PF-14: Some log lines are missing their `key=value` fields
- `storage_stop` lacks the segment and last `seq` that DESIGN.md's new text says it carries.
- `storage_backlog` doesn't say which threshold (50/75/90%) was crossed.
- Only the newest segment's recovery is logged, so an older segment truncated during the walk back for the last `seq` goes unreported.

### PF-15: Doc errors
- **[Both docs]** The Hub range-check row says "light 0–65535 **lux**". Sensor-Pipeline.md says "Raw counts are stored, never lux", and its Step 7 changed this row to "raw count". Git history shows the fix was lost in merge #71 (`complete-pipe` → `main`), not by Lucas.
- The Storage "malformed message" row says `gt_msg_decode()` detects it; storage actually uses `gt_record_decode`.
- **Resolution (2026-10-26):**
  - **Range-check row:** the Hub row reads "light raw count 0–65535" again, the wording Sensor-Pipeline.md's Step 7 had put in. Sensor-Pipeline.md itself already said raw counts, so only DESIGN.md changed.
  - **Malformed-message row:** the Storage row now names `gt_record_decode()`.

## Contradicts Sensor-Pipeline.md only

### PF-16: Socket calls outside `src/ipc_sock.c`
- **Sensor-Pipeline.md** (*Design principle*): "`src/ipc_sock.c` is the only file that makes socket system calls"; everything else goes through `gt_sock_ops` so tests can use a fake.
- `listen_sock.c` calling `socket` / `bind` / `listen` is expected: the plan's own *Decisions* asked for that shared helper.
- `storage_server.c` calls `accept`, `recv` and `send` directly ([src/storage_server.c:79](src/storage_server.c#L79), [96](src/storage_server.c#L96), [115](src/storage_server.c#L115)), so its loop is tested only over real sockets (SSV), not with a fake. Defensible for a server, but it should be recorded as a Decision.

### PF-17: Lily's Status section edited; "storage is done" overclaims
- **Sensor-Pipeline.md**: "Update the Status section only at the end of the day, when asked." The PR rewrites Lily's *Not done yet* bullet to say "**The storage process itself is done**".
- Lucas's own *Not done yet (Mechanism D)* still lists disk full, read-only filesystem, the heartbeat, and the fault and power-pull runs.
- The file's opening paragraph still scopes it to "the 24 B record the hub sends to storage", but it now ends with a second top-level section with its own Status, *Not done yet* and Log, so "update Status" no longer says which one.

### PF-18: The log sink was chosen by one side
- **Sensor-Pipeline.md** (*Decisions → Log sink deferred*): where log lines go is to be decided together with the light process.
- `storage_main` writes to stderr ([src/storage_main.c:68-75](src/storage_main.c#L68-L75)). A reasonable choice, but it now sets the precedent for every process.

### PF-19: The Code map is out of date
- The diagram still shows the storage process as not built (☐).
- The "storage rx thread" row says `gt_record_decode → gt_storage_append(gt_record)`; `gt_storage_append` now takes the raw 24 B and decodes them itself, and `gt_storage_open` has a new signature.

## Smaller storage edge cases

### PF-20
- **`.corrupt` files:** retention never deletes them, a second `.corrupt` of the same name silently replaces the first (`renameat`), and once the directory holds 128 names (`GT_FILE_LIST_MAX`), `gt_storage_open` fails.
- **Rollover failure logs every second:** while a new segment can't be created, `segment_create_error` is logged on every 1 s tick.
- **The 1970 file:** a Pi that boots with no network time and an empty log writes `log-19700101.bin` until NTP syncs (acknowledged in Lucas's *Not done yet*).

## What meets the plans

**DESIGN.md**
- **`storage_main.c` start-up order:** the one-listener lock, then recovery, then bind `storage.sock`, then the flush thread, then the rx loop. A second copy refuses to start.
- **Shutdown:** on SIGTERM, storage takes any records already in the socket, joins the flush thread, does the final flush, and writes the clean-shutdown marker only if that flush succeeded. The stop signals are blocked in the flush thread, SIGPIPE is ignored, and the exit status is honest. Seen working in the PF-04 run.
- **Recovery:** marker check, newest-segment header check and `.corrupt` rename, scan to the first bad record (magic, type, version, CRC, `seq`, partial tail), truncate + fsync, then the walk back for the last `seq`.
- **Durability:** fsync per batch; directory fsync when a segment is created; on a failed write or fsync the batch stays pending, storage is marked degraded, and the next cycle truncates to the last committed offset before rewriting.
- **Backlog and refusals:** `storage_backlog` at 50/75/90%; when full, refuse and count while still reading the socket; the next accepted record brings a GAP whose `seq` is the first refused reading.
- **Retention:** 30 days, deleting only outside the window and beyond the newest 30 segments; skipped while the clock is unsynced.
- **Locking:** the mutex is never held during file I/O.
- **Documentation brought in line:** the single pending buffer and memory budget, the commit sequence, recovery step 5, the retention rule, `storage.sock` as `SOCK_SEQPACKET`, and the `seq_resume` layout.
- **Verifier:** read-only, checks `seq` order across segment boundaries, its GAP rule matches what append writes, and it counts `.corrupt` files without failing on them (VFY, 10 tests).
- **Logging:** every line goes through `gt_log_format` with µs wall-clock timestamps.

**Sensor-Pipeline.md**
- **Lock helper:** the one-listener lock moved into a shared `src/` helper (`listen_sock.c`) with `fork()`-based tests (LCK), exactly as *Decisions* planned. `hub_listen` behaves as before.
- **Hop 2:** the 24 B record is checked with `gt_record_decode` and appended as the raw bytes received, per the 2026-10-23 decision. A storage GAP uses `sensor_id 0`, which the wire-format section allows.
- **Wrong-length frames:** storage receives into a 25 B buffer and closes the connection on a wrong length (`ipc_bad_message`), mirroring the hub's 17 B rule.
- **Same patterns:** events returned rather than logged, time passed in, a file seam with a fake for tests, `MSG_NOSIGNAL`, `SOCK_SEQPACKET`.
- **Test ID prefixes:** none of the 23 new prefixes collide with the existing ones.

## Recommended before merge

1. ~~**Decide PF-01 to PF-03 in DESIGN.md:** the hub stamps the timestamp on arrival and `seq` + CRC at send time; the socket seam gains a `recv` op and the sender a "waiting for `seq_resume`" state; a running hub keeps its own `seq` counter. Update Sensor-Pipeline.md's *Decisions* and `include/hub.h` to match.~~ **Done in both documents (2026-10-26).** `include/hub.h` waits for the hub steps.
2. ~~**Fix PF-04** (and PF-05), then run `storage_faults.sh sigterm 20` and `kill9 50` and record the results.~~ **Done (2026-10-26):** both ran with 0 failed checks; results are in PF-04/05 and the Mechanism D Status table.
3. ~~**Fix or document PF-06 and PF-07.**~~ **Done (2026-10-26):** both fixed test-first; see their Resolution lines.
4. ~~**Mark PF-09's rows as planned**~~ (done 2026-10-26) and settle PF-10's heartbeat owner thread.
5. **Clean up the docs:** ~~PF-15 (restore "raw count")~~ (done 2026-10-26), PF-17 (Status ownership and file scope), PF-19 (Code map).
