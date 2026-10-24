# Sensor Pipeline: plan and status

The official plan for Green Thumb's sensor-side pipeline, from the I2C seam up to the 16 B message a sensor process sends to the hub and the 24 B record the hub sends to storage. That covers the BH1750 light-sensor driver and the shared pieces it relies on: sensor health tracking, the log line formatter, the sensor→hub message and the hub→storage record codec. A new session should be able to read this file, plus `CLAUDE.md` and `DESIGN.md`, and pick up where work stopped without re-explanation. **Update the Status section only at the end of the day, when asked.**

## Context
- **Owner:** Lily (DESIGN.md → Ownership map: "Light sensor (driver and process)" and "IPC (data path)").
- **Goal of this phase:** prove the sensor is *electrically alive* and *trustworthy* before it is connected to the data pipeline. That means the bus opens, the chip ACKs, reads decode, failures are retried and bounded, and a sensor that keeps failing is marked offline instead of silently producing nothing. Then pin the wire message the light process will send.
- **Hardware:** BH1750 on `/dev/i2c-1` at address `0x23` (ADDR pin low or floating). Continuous high-res mode at 1 lx/count; each conversion takes at most 180 ms. Lux = raw / 1.2. **Raw counts are stored, never lux.**
- **Sampling rate:** 1 Hz (DESIGN.md). The 1 Hz schedule belongs to the sampler, not the driver.
- **Rule from DESIGN.md's failure-mode table (Sensor (I2C) row):** retry 3× at 10 ms, then skip the sample. After 10 consecutive failures, mark the sensor offline and retry every 5 s. Log `i2c_read_error`, `sensor_offline` and `sensor_online`.

## Design principle
- **Layered seam:** `src/i2c_bus.c` is the only file that makes I2C system calls. The driver talks to the bus only through `gt_i2c_ops` (`write` / `read` / `sleep_ms` / `close`). Tests swap in `tests/fake_i2c.h`, so every driver path, including injected NACKs and short reads, runs in WSL/CI with no hardware.
- **Logic stays separate from hardware** (CLAUDE.md): anything that can be pure logic (health tracking, message encoding) takes its inputs as arguments (`gt_status`, `now_ms`, bytes) and makes no system calls.
- **Each layer fails explicitly:** a failure returns a `gt_status` and leaves outputs untouched. No layer invents a value.

### The "electrically alive" ladder
Each rung can only be checked once the rung below it passes. `light_probe` prints rungs 1–4 as a PASS/FAIL checklist on the Pi.

| Rung | What it proves | Where it's checked | Detected by |
|---|---|---|---|
| 1 | The I2C bus device opens (I2C enabled, permissions OK) | `light_probe` check 1; LSO-02 | `open()` fails → ENOENT / EACCES hint |
| 2 | A BH1750 ACKs at 0x23 (wiring, ADDR pin, power) | `light_probe` check 2; LSO-03/04 | First write (POWER_ON) is NACKed |
| 3 | One measurement reads and decodes | `light_probe` check 3; LSR-01..03 | 2-byte read, high byte first |
| 4 | Sampling holds 1 Hz with p99 jitter < 10 ms | `light_probe` check 4 | `CLOCK_MONOTONIC` deviation from an absolute schedule |
| 5 | A sensor that keeps failing is marked offline, probed every 5 s, and announced when it comes back | **Steps 1–3 (health tracker + log line)** | 10 consecutive failed reads |

### Open: what each failure point has acquired, and what must be unwound
`gt_light_open_bus` takes ownership of the bus, so the caller never cleans up:

| Fails at | Acquired so far | Unwound | Returns |
|---|---|---|---|
| `bus.ops == NULL` | nothing | nothing | `NULL` |
| `malloc` of driver state | the bus | `bus.ops->close` | `NULL` |
| POWER_ON / RESET / CONT_HRES write | bus + state | close the bus once, `free(s)`, no settle wait | `NULL` |
| after CONT_HRES | — | waits 180 ms so the first read returns real data | handle |

### Read: budget
In the worst case a read makes 1 try + 3 retries with 3 × 10 ms gaps, so it waits 30 ms, far inside the 1 s period. On failure the function returns `GT_ERR_IO` with `raw_out` untouched, so the caller skips the sample (LSR-07). The same handle works again after an outage, with no reopen (LSR-09).

## Decisions
- **The health tracker is generic** (`sensor_health`), not light-only, so the soil process can reuse it. It is a small public struct with no malloc.
- **Health input:** any non-`GT_OK` result from `gt_light_read` counts as one failure. The driver's 3 retries are already inside a single failed read.
- **Health output:** `gt_health_record` returns a transition event (`NONE` / `OFFLINE` / `ONLINE`) and the caller logs it. The tracker does no I/O.
- **Time is passed in** as `now_ms`, matching `gt_sampler_tick`, so tests need no clock.
- **Test ID prefixes:** `SHR` (sensor health record), `SHS` (sensor health should-read), `SHE` (sensor health event name), `LGF` (log format), `MSE` (message encode), `MSD` (message decode), `CRC` (CRC-32), `RCE` (record encode), `RCD` (record decode).
- **Log ownership:** the light sensor's events (`i2c_read_error`, `sensor_offline`, `sensor_online`) are Lily's, as the light process's read-failure handling. The shared formatter (`gt_log_format`) has no owner in the Ownership map because every component uses it, so it counts as **Joint → Architecture integration**. Lily writes it first, as its first caller; give Lucas a heads-up so his components reuse it.
- **Log line:** `<timestamp_us> <LEVEL> <component> <event>[ key=value ...]`. The timestamp is µs since the Unix epoch, as a plain integer, the same unit as the on-disk record, so logs and records can be joined. The levels are `DEBUG` / `INFO` / `WARN` / `ERROR`. The light process logs `sensor_offline` as WARN and `sensor_online` as INFO.
- **Log sink deferred:** the formatter only fills a caller-provided buffer. Where lines go (stderr, a file or the monitor process) and reading the wall clock are decided with the light process. DESIGN.md doesn't specify either.
- **Hop 2 (hub→storage) carries the 24 B on-disk record** (decided 2026-10-23). The hub stamps `seq` and the µs timestamp, builds the record (CRC included), and storage checks it with `gt_record_decode`, then appends it unchanged. There are two reasons. First, DESIGN.md already has the hub stamping the time and storage reading *records* from `storage.sock`. Second, a record that waits in the hub's 1024-record queue during a storage outage (up to about 17 min) keeps its real receive time. If storage stamped the time instead, the timestamp would be off by however long the record waited.
- **Message magic is `0xA6`** (proposed), distinct from the on-disk record's `0xA7`, so a record can never be misread as a message.
- **Truncated messages are rejected at `recv()`,** which must return exactly `GT_MSG_SIZE`, not in decode. Decode keeps its fixed 16 B array.
- **Range checks stay in the hub:** decode doesn't check that light ≤ 65535. That is the hub's `reading_invalid` check.
- **Deferred:** `src/light_sensor_main.c` (open → sample → health → send) waits until the IPC client exists.

## Wire format: sensor → hub message (16 B, little-endian, encoded byte by byte)
| Off | Size | Field | Value |
|---|---|---|---|
| 0 | 1 | magic | `GT_MSG_MAGIC` = `0xA6` |
| 1 | 1 | version | `GT_MSG_VERSION` = 1 |
| 2 | 1 | type | `GT_MSG_DATA` = 1, `GT_MSG_GAP` = 2 (same values as `gt_rec_type`) |
| 3 | 1 | sensor_id | 1 light, 2 soil (0 = all is not valid on hop 1) |
| 4 | 4 | counter | sensor-side counter (for a GAP: the first refused counter) |
| 8 | 4 | seq | 0 from the sensor; the hub stamps it |
| 12 | 4 | value | DATA: raw count; GAP: refused-reading count |

Magic and version are written by encode and checked by decode. They are not `gt_msg` fields; `type` becomes one.

## Wire format: hub → storage record (24 B)
Hop 2 sends the on-disk record exactly as DESIGN.md *In what format → Record* lays it out: magic `0xA7`, type, version, sensor_id, seq (u32 at 4), timestamp (u64 µs at 8), value (u32 at 16) and CRC-32 over bytes 0–19 (at 20). `gt_record` and the codec live in `include/log_record.h`. **Watch the header order:** the record goes magic, **type, version**, sensor_id, while the message goes magic, **version, type**, sensor_id. Don't copy the message encoder without swapping those two bytes. Unlike the message, a GAP record may use sensor_id 0 (all).

## Health state machine
```
          10th consecutive failure → EV_OFFLINE, next_probe = now + 5000
 ONLINE ───────────────────────────────────────────────────▶ OFFLINE
   ▲  success → reset count                                     │ should_read() true only when now ≥ next_probe
   │  failure → count++ (no event before 10)                    │ failed probe → EV_NONE, next_probe = now + 5000
   └────────────────────── success → EV_ONLINE, count = 0 ◀─────┘
```

## Steps
**Workflow** (CLAUDE.md and the user's preferences):
- One step per turn. Stop after each step for review; the user commits.
- Tests come first (red), then the implementation (green).
- A step is **done** only when `make test`, `make asan` and `make memcheck` all pass from WSL, with the output shown.
- Never weaken, skip or delete a test to make the suite pass.

### Step 0: foundation (done)
- **I2C seam:** `include/i2c_bus.h`, `src/i2c_bus.c` (Linux `open` + `ioctl(I2C_SLAVE)`, with EINTR handling).
- **Driver:** `include/light_sensor.h`, `src/light_sensor.c` (open/read/close, and the retry policy).
- **Test infrastructure:** `tests/check.h` (`CHECK`, `CHECK_EQ`, `RUN_TESTS`) and `tests/fake_i2c.h` (scripted reads, `fail_writes` / `fail_reads` / `fail_write_at`).
- **Tests (18):** `test_light_open.c` LSO-01..06, `test_light_read.c` LSR-01..10, `test_light_close.c` LSC-01..02.
- **On-Pi tool:** `src/light_probe_main.c` (`make light_probe`), which runs rungs 1–4.

### Step 1: health tracker header and tests (red)
- **New `include/sensor_health.h`:** `GT_HEALTH_OFFLINE_AFTER 10`, `GT_HEALTH_RETRY_MS 5000`, `gt_health_state`, `gt_health_event`, a `gt_health` struct (`state`, `consecutive_fails`, `next_probe_ms`), and three functions:
  - `void gt_health_init(gt_health *h);`
  - `int gt_health_should_read(const gt_health *h, uint64_t now_ms);`: online → 1; offline → 1 only once `now_ms ≥ next_probe_ms`; NULL → 0.
  - `gt_health_event gt_health_record(gt_health *h, gt_status st, uint64_t now_ms);`: NULL → `EV_NONE`.
- **New `tests/test_health_record.c`:**
  - SHR-01 init: online, 0 failures
  - SHR-02 9 consecutive failures stay online, no event
  - SHR-03 the 10th consecutive failure returns OFFLINE, exactly once
  - SHR-04 a success resets the run (9 fail, 1 ok, 9 fail → still online)
  - SHR-05 a failure while offline returns NONE (no repeated offline event)
  - SHR-06 a success while offline returns ONLINE and resets the count
  - SHR-07 a NULL tracker returns NONE without crashing
- **New `tests/test_health_should_read.c`:**
  - SHS-01 online → always true
  - SHS-02 offline at t → false at t+4999, true at t+5000
  - SHS-03 a failed probe while offline reschedules to probe_time + 5000
  - SHS-04 back online → true immediately
  - SHS-05 NULL → false
- **Expected red state:** the two new binaries fail to link. LSO/LSR/LSC still build and pass.

### Step 2: health tracker implementation (green)
- **New `src/sensor_health.c`:** about 40 lines, no system calls. The header comment cites the DESIGN.md failure-mode row. The Makefile's `LIB_SRC` wildcard picks it up automatically.
- **Done when:** all three targets pass, SHR 7/7 and SHS 5/5.

### Step 3: event-to-log path (3a red, 3b green)
This completes rung 5 end to end: the driver detects the failure, the tracker turns it into a transition, and the formatter turns that into a visible line. It depends on nothing from the IPC steps, and they depend on nothing here.
- **3a, new `include/gt_log.h`:** `gt_log_level` (`DEBUG`/`INFO`/`WARN`/`ERROR`), `GT_LOG_LINE_MAX 256`, and `gt_status gt_log_format(char *line_out, size_t line_size, uint64_t timestamp_us, gt_log_level level, const char *component, const char *event, const char *fields);`.
  - The output is always NUL-terminated.
  - `fields` is a caller-built `"key=value ..."` string, or NULL/"" for none.
  - It returns `GT_ERR_INVAL` for a NULL `line_out`/`component`/`event`, an unknown level, or a space inside `component`/`event`, and `GT_ERR_FULL` when the line doesn't fit.
- **3a, add to `include/sensor_health.h`:** `const char *gt_health_event_name(gt_health_event event);`, which returns `"sensor_offline"` or `"sensor_online"`, and NULL for NONE or an unknown value.
- **3a, new `tests/test_log_format.c`:**
  - LGF-01 a full line (`1700000000000000 WARN light_sensor sensor_offline fails=10`)
  - LGF-02 NULL or "" fields → no trailing space
  - LGF-03 each level prints its name
  - LGF-04 timestamps 0 and `UINT64_MAX` print in full
  - LGF-05 a buffer that is too small → `GT_ERR_FULL`, still NUL-terminated
  - LGF-06 NULL arguments → `GT_ERR_INVAL`
  - LGF-07 a space inside `component`/`event` → `GT_ERR_INVAL`
  - LGF-08 an unknown level → `GT_ERR_INVAL`
- **3a, add to `include/sensor_health.h`:** `#define GT_HEALTH_I2C_READ_ERROR "i2c_read_error"`. The process logs this name on every failed read. It is not a tracker event, so `gt_health_record` and the SHR/SHS tests are unchanged.
- **3a, new `tests/test_health_event_name.c`:** SHE-01 OFFLINE → `"sensor_offline"` · SHE-02 ONLINE → `"sensor_online"` · SHE-03 NONE → NULL · SHE-04 an out-of-range value → NULL · SHE-05 `GT_HEALTH_I2C_READ_ERROR` is `"i2c_read_error"`.
- **3a, expected red state:** the two new binaries fail only on undefined references. Everything else stays green.
- **3b, new `src/gt_log.c`:** one `snprintf` plus a level → name table, with no system calls. A result `>= line_size` means the line didn't fit.
- **3b, edit `src/sensor_health.c`:** add `gt_health_event_name`.
- **Done when:** all three targets pass, LGF 8/8 and SHE 5/5.

### Step 4: message layout and tests (red)
- **Edit `include/ipc.h`:** add `GT_MSG_MAGIC`, `GT_MSG_VERSION`, `GT_MSG_DATA` and `GT_MSG_GAP`, add a `uint8_t type` field to `gt_msg`, and add the layout comment from the table above. The `gt_msg_encode/decode` signatures stay as they are.
- **New `tests/test_msg_encode.c`:**
  - MSE-01 `GT_MSG_SIZE == 16`
  - MSE-02 header bytes 0–3 (magic, version, type, sensor_id)
  - MSE-03 the u32 fields are little-endian at offsets 4/8/12 (distinctive values such as `0x04030201`)
  - MSE-04 a GAP message encodes
  - MSE-05 an unknown type is rejected and `out` is untouched
  - MSE-06 sensor_id 0 or 3 is rejected
  - MSE-07 NULL args → `GT_ERR_INVAL`
- **New `tests/test_msg_decode.c`:**
  - MSD-01 round trip for DATA and GAP at the 0 and `UINT32_MAX` edges
  - MSD-02 decode of hand-built bytes (does not depend on encode)
  - MSD-03 bad magic → `GT_ERR_CORRUPT`, `m` untouched
  - MSD-04 bad version → `GT_ERR_CORRUPT`
  - MSD-05 unknown type → `GT_ERR_CORRUPT`
  - MSD-06 bad sensor_id → `GT_ERR_CORRUPT`
  - MSD-07 NULL args → `GT_ERR_INVAL`
- **Expected red state:** the two new binaries fail to link. Everything else stays green.

### Step 5: message implementation (green)
- **New `src/ipc_msg.c`:** `gt_msg_encode` / `gt_msg_decode` with byte-wise little-endian put/get helpers. The socket client goes in a separate `src/ipc_client.c` later. The `gt_ipc_client_*` declarations stay unimplemented, which is safe because nothing calls them yet.
- **Done when:** all three targets pass, MSE 7/7 and MSD 7/7.

### Step 6: docs
- **DESIGN.md:** add the 16 B sensor→hub layout table, and one line recording that the hop-2 format is still undecided, and why. Next to the log format (above the *Failure-mode table*), add one line recording the timestamp unit (µs since epoch, integer) and the level names.
- **Notes-with-Claude.md:** add one 2–4 sentence blurb each for the health tracker, the log line and the message layout.
- **This file:** update Status.

### Step 7: hop-2 docs and headers
- **DESIGN.md:**
  - *Storage and data → At what rate* ("On the wire"): hop 2 carries the 24 B record; the hub stamps `seq` and the time; storage checks it.
  - *In what format*, under the sensor → hub message table: "Open:" becomes "Decided:", with the reason.
  - *Constraints → Resource budget*: hub queue `1024 records × 24 B | ≈24 KB`.
  - *Constraints → IPC overhead*: "16 B messages sensor → hub and 24 B records hub → storage".
  - *Evaluation plan → IPC latency*: "a 16 B message (sensor → hub) and a 24 B record (hub → storage)".
  - *Ownership map → IPC (data path)*: adds "the 24 B record on `storage.sock`".
  - Consistency fixes found along the way: the Hub range check reads "light raw count 0–65535" (not lux), the stray assignment-prompt line at the top is removed, and the duplicated AI-usage heading is merged.
- **`include/ipc.h:23`:** replace the "still undecided" comment with "Hub → storage sends the 24 B record (log_record.h)."
- **`include/hub.h:11`:** `gt_hub_accept(..., gt_record *out)`, include `log_record.h`, and update the comment. Nothing calls it yet, so the build stays green.
- **`include/log_record.h`:** add a layout comment like the one in `ipc.h` (offsets 0–23, CRC over bytes 0–19, written byte by byte), plus short comments on each function.
- **Notes-with-Claude.md:** one 2–4 sentence blurb on why hop 2 sends the record.
- **Done when:** all three targets pass, with 57 tests unchanged.

### Step 8: record codec tests (red)
- **New `tests/test_crc32.c`:**
  - CRC-01 `"123456789"` → `0xCBF43926` (the standard CRC-32 check value)
  - CRC-02 a zero-length input → `0x00000000`
  - CRC-03 changing one bit changes the CRC
- **New `tests/test_record_encode.c`:**
  - RCE-01 `GT_REC_SIZE == 24`
  - RCE-02 header bytes 0–3 are magic, **type, version**, sensor_id
  - RCE-03 seq at 4, timestamp at 8 (u64), value at 16, all little-endian (distinctive values)
  - RCE-04 bytes 20–23 equal `gt_crc32(out, 20)`, little-endian
  - RCE-05 a GAP record with sensor_id 0 (all) encodes
  - RCE-06 an unknown type is rejected and `out` is untouched
  - RCE-07 DATA with sensor_id 0, or any sensor_id 3, is rejected
  - RCE-08 NULL args → `GT_ERR_INVAL`
- **New `tests/test_record_decode.c`:**
  - RCD-01 round trip for DATA and GAP at the 0 and max edges (timestamp `UINT64_MAX`)
  - RCD-02 decode of hand-built bytes, including a hand-computed CRC
  - RCD-03 bad magic (`0xA6`, a message) → `GT_ERR_CORRUPT`, `rec` untouched
  - RCD-04 bad version → `GT_ERR_CORRUPT`
  - RCD-05 unknown type → `GT_ERR_CORRUPT`
  - RCD-06 bad sensor_id → `GT_ERR_CORRUPT`
  - RCD-07 a CRC mismatch (one payload byte flipped) → `GT_ERR_CORRUPT`
  - RCD-08 NULL args → `GT_ERR_INVAL`
- **Expected red state:** the three new binaries fail only on undefined references. Everything else stays green.

### Step 9: record codec implementation (green)
- **New `src/log_record.c`:** a bitwise reflected CRC-32 (polynomial `0xEDB88320`, init and final XOR `0xFFFFFFFF`, no lookup table), plus encode and decode using byte-wise little-endian put/get helpers, as in `src/ipc_msg.c`. Decode checks the CRC before trusting any field. No system calls.
- **Done when:** all three targets pass, CRC 3/3, RCE 8/8 and RCD 8/8.

### Later (not scheduled)
- **Hardware check:** run `make light_probe` on the Pi and record the rung 1–4 results and jitter numbers under Status.
- **IPC client** (`gt_ipc_client_*`, with reconnect at 100 ms → 2 s backoff and a 64-reading ring).
- **Hub implementation:** `gt_hub_accept` validates a message, stamps `seq` and the time, and builds the `gt_record` it forwards.
- **`src/light_sensor_main.c`:** sampler at 1000 ms + health tracker + IPC client, plus a heartbeat to `sup.sock`. Also the log sink (stderr, a file or the monitor process) and wall-clock timestamps for log lines.

## Files
- **Done (Step 0):** `include/i2c_bus.h`, `src/i2c_bus.c`, `include/light_sensor.h`, `src/light_sensor.c`, `src/light_probe_main.c`, `tests/check.h`, `tests/fake_i2c.h`, `tests/test_light_{open,read,close}.c`
- **New (Steps 1–5):** `include/sensor_health.h`, `src/sensor_health.c`, `include/gt_log.h`, `src/gt_log.c`, `src/ipc_msg.c`, `tests/test_health_record.c`, `tests/test_health_should_read.c`, `tests/test_health_event_name.c`, `tests/test_log_format.c`, `tests/test_msg_encode.c`, `tests/test_msg_decode.c`
- **Modified:** `include/sensor_health.h` and `src/sensor_health.c` (Step 3), `include/ipc.h` (Step 4), `DESIGN.md` and `Notes-with-Claude.md` (Step 6)
- **Steps 7–9:** modified `DESIGN.md`, `include/ipc.h`, `include/hub.h`, `include/log_record.h` and `Notes-with-Claude.md` (Step 7); new `tests/test_crc32.c`, `tests/test_record_encode.c` and `tests/test_record_decode.c` (Step 8); new `src/log_record.c` (Step 9)
- **Untouched:** `Makefile` (the wildcards cover the new files), the driver and the seam

## Verification
```
wsl -e bash -lc 'cd /mnt/c/Users/lheer/OneDrive/Desktop/cs370/CS370_TermProject/The-Green-Thumb && make test && make asan && make memcheck'
```
- **After Steps 1, 3a, 4 and 8 (red):** only the new binaries fail, and only because they don't link.
- **After Step 7 (docs and headers):** the 57 existing tests still pass, unchanged.
- **After Steps 2, 3b, 5 and 9 (green):** every binary prints `N/N tests passed`, `-Wall -Wextra -Werror` stays clean, and Valgrind reports 0 errors and 0 bytes in use.
- If anything else fails, stop and report. Fix `src/`, never the tests.

## Status
_Update at the end of the day, when asked: tick the step table, and add one Log entry with the date, what landed, test counts, and anything surprising._

| Step | What | State |
|---|---|---|
| 0 | Seam, driver, 18 tests, light_probe | ✅ Done (branch `adapt_test_structure`, through `a74ca65`) |
| 1 | Health header + SHR/SHS tests (red) | ✅ Done, red confirmed (2026-10-23) |
| 2 | `src/sensor_health.c` (green) | ✅ Done (2026-10-23): SHR 7/7, SHS 5/5 |
| 3 | Log formatter + event names (3a red, 3b green) | ✅ Done (2026-10-23): LGF 8/8, SHE 5/5 |
| 4 | `ipc.h` layout + MSE/MSD tests (red) | ✅ Done, red confirmed (2026-10-23) |
| 5 | `src/ipc_msg.c` (green) | ✅ Done (2026-10-23): MSE 7/7, MSD 7/7 |
| 6 | DESIGN.md, notes, this file | ✅ Done (2026-10-23) |
| 7 | Hop-2 docs and headers (`ipc.h`, `hub.h`, `log_record.h`) | ✅ Done (2026-10-23): 57 tests unchanged |
| 8 | CRC/RCE/RCD tests (red) | ✅ Done, red confirmed (2026-10-23): failed only on undefined references |
| 9 | `src/log_record.c` (green) | ✅ Done (2026-10-23): CRC 3/3, RCE 8/8, RCD 8/8; 76 tests across 12 binaries, 0 Valgrind errors |
| — | On-Pi `light_probe` run | ☐ Not recorded |
| — | Hop-2 (hub→storage) format | ✅ Decided 2026-10-23: the 24 B record (see Decisions) |

### Not done yet
These are what's left before a reading can travel from the light sensor to storage:
- **IPC client** (`src/ipc_client.c`, not started): implements the `gt_ipc_client_open` / `_send` / `_close` functions declared in `ipc.h`. It reconnects with 100 ms → 2 s backoff, buffers up to 64 readings in a ring, sends a GAP message after an overflow, and logs `ipc_disconnect`, `ipc_reconnect` and `ipc_overflow`. Its `recv()` side must reject anything shorter than `GT_MSG_SIZE`. Hop 1 is done, so nothing blocks this.
- **`gt_hub_accept`** (no `src/hub.c` yet): its signature was updated in Step 7 to output a `gt_record`, but it isn't implemented. It has to validate the message (including the `reading_invalid` range check), stamp `seq` and the µs receive time, and build the record that `gt_record_encode` turns into bytes for `storage.sock`.
- **`src/light_sensor_main.c`** (not started): the 1000 ms sampler, the health tracker and the IPC client, plus a heartbeat to `sup.sock`. This is also where the log sink (stderr, a file or the monitor process) and wall-clock timestamps for log lines get decided. It depends on the IPC client.
- **On-Pi `light_probe` run** (not recorded): `make light_probe` on the Pi with the BH1750 attached. Record the rung 1–4 PASS/FAIL results and the jitter numbers in the table above. It needs real I2C, so it can't run in WSL or CI.

### Log
- **2026-10-23:** Plan written. Step 0 is complete as committed. The last commit says the test structure was verified; this session did not re-run the suite. Next: Step 1.
- **2026-10-23 (end of day):** Steps 1–9 landed:
  - The health tracker (SHR 7/7, SHS 5/5).
  - The log formatter and event names (LGF 8/8, SHE 5/5).
  - The 16 B sensor→hub message (MSE 7/7, MSD 7/7).
  - The 24 B record codec with CRC-32 (CRC 3/3, RCE 8/8, RCD 8/8).

  `make test`, `make asan` and `make memcheck` all pass: 76 tests across 12 binaries, 0 Valgrind errors, 0 bytes in use. Hop 2 was decided as the 24 B record, because the hub owns the timestamp and records can wait up to about 17 min in its queue during a storage outage.

  Surprises:
  - DESIGN.md had drifted in four places: 16 B was assumed on both hops, the range check said lux instead of raw count, a stray prompt line sat at the top, and the AI-usage heading appeared twice. All four are fixed.
  - The RCD tests have to recompute the CRC after breaking a header byte. Otherwise the CRC check alone rejects every case, and the magic/version/type/sensor checks never run.
  - In a red step, plain `make test` stops at the first link failure, so the existing binaries were built and run separately to confirm they stayed green.

  Next: the IPC client (see *Not done yet*).
