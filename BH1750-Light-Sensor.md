# BH1750 Light Sensor: plan and status

The official plan for the BH1750 light-sensor path in Green Thumb, from the I2C seam up to the 16 B message the light process will send to the hub. A new session should be able to read this file, plus `CLAUDE.md` and `DESIGN.md`, and pick up where work stopped without re-explanation. **Update the Status section only at the end of the day, when asked.**

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
| 5 | A sensor that keeps failing is marked offline, probed every 5 s, and announced when it comes back | **Steps 1–2 (health tracker)** | 10 consecutive failed reads |

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
- **Test ID prefixes:** `SHR` (sensor health record), `SHS` (sensor health should-read), `MSE` (message encode), `MSD` (message decode).
- **Today we define only the sensor→hub message (hop 1).** The hub→storage format (hop 2) is **undecided**: DESIGN.md says 16 B on both hops, but a hub-stamped µs timestamp needs at least 20 B. The options are sending the 24 B on-disk record on hop 2, or having storage stamp the time.
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

### Step 3: message layout and tests (red)
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

### Step 4: message implementation (green)
- **New `src/ipc_msg.c`:** `gt_msg_encode` / `gt_msg_decode` with byte-wise little-endian put/get helpers. The socket client goes in a separate `src/ipc_client.c` later. The `gt_ipc_client_*` declarations stay unimplemented, which is safe because nothing calls them yet.
- **Done when:** all three targets pass, MSE 7/7 and MSD 7/7.

### Step 5: docs
- **DESIGN.md:** add the 16 B sensor→hub layout table, and one line recording that the hop-2 format is still undecided, and why.
- **Notes-with-Claude.md:** add one 2–4 sentence blurb for the health tracker and one for the message layout.
- **This file:** update Status.

### Later (not scheduled)
- **Hardware check:** run `make light_probe` on the Pi and record the rung 1–4 results and jitter numbers under Status.
- **Hop-2 decision,** then the IPC client (`gt_ipc_client_*`, with reconnect at 100 ms → 2 s backoff and a 64-reading ring).
- **`src/light_sensor_main.c`:** sampler at 1000 ms + health tracker + IPC client, plus a heartbeat to `sup.sock`.

## Files
- **Done (Step 0):** `include/i2c_bus.h`, `src/i2c_bus.c`, `include/light_sensor.h`, `src/light_sensor.c`, `src/light_probe_main.c`, `tests/check.h`, `tests/fake_i2c.h`, `tests/test_light_{open,read,close}.c`
- **New (Steps 1–4):** `include/sensor_health.h`, `src/sensor_health.c`, `src/ipc_msg.c`, `tests/test_health_record.c`, `tests/test_health_should_read.c`, `tests/test_msg_encode.c`, `tests/test_msg_decode.c`
- **Modified:** `include/ipc.h` (Step 3), `DESIGN.md` and `Notes-with-Claude.md` (Step 5)
- **Untouched:** `Makefile` (the wildcards cover the new files), the driver and the seam

## Verification
```
wsl -e bash -lc 'cd /mnt/c/Users/lheer/OneDrive/Desktop/cs370/CS370_TermProject/The-Green-Thumb && make test && make asan && make memcheck'
```
- **After Steps 1 and 3 (red):** only the new binaries fail, and only because they don't link.
- **After Steps 2 and 4 (green):** every binary prints `N/N tests passed`, `-Wall -Wextra -Werror` stays clean, and Valgrind reports 0 errors and 0 bytes in use.
- If anything else fails, stop and report. Fix `src/`, never the tests.

## Status
_Update at the end of the day, when asked: tick the step table, and add one Log entry with the date, what landed, test counts, and anything surprising._

| Step | What | State |
|---|---|---|
| 0 | Seam, driver, 18 tests, light_probe | ✅ Done (branch `adapt_test_structure`, through `a74ca65`) |
| 1 | Health header + SHR/SHS tests (red) | ✅ Done, red confirmed (2026-10-23) |
| 2 | `src/sensor_health.c` (green) | ☐ Not started |
| 3 | `ipc.h` layout + MSE/MSD tests (red) | ☐ Not started |
| 4 | `src/ipc_msg.c` (green) | ☐ Not started |
| 5 | DESIGN.md, notes, this file | ☐ Not started |
| — | On-Pi `light_probe` run | ☐ Not recorded |
| — | Hop-2 (hub→storage) format | ☐ Undecided |

### Log
- **2026-10-23:** Plan written. Step 0 is complete as committed. The last commit says the test structure was verified; this session did not re-run the suite. Next: Step 1.
