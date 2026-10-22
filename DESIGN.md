3-5 pages and the most leveraged hours of the whole project. Because every hour of arguing here saves five of rewriting later. Required contents:

## Green-Thumb Architecture Diagram

```mermaid
flowchart TB

    subgraph HW["Physical hardware"]
        LIGHT["BH1750 light sensor<br/>I2C addr 0x23"]
        SOIL["Adafruit STEMMA soil sensor<br/>I2C addr 0x36 (seesaw)"]
        SD[("microSD card<br/>ext4")]
    end

    subgraph KERNEL["Kernel space"]
        I2C["I2C subsystem<br/>i2c-bcm2835 · /dev/i2c-1"]
        HUBSOCK["UDS · AF_UNIX<br/>/run/plantpi/hub.sock"]
        STOSOCK["UDS · AF_UNIX<br/>/run/plantpi/storage.sock"]
        SUPSOCK["UDS · AF_UNIX<br/>/run/plantpi/sup.sock"]
        VFS["VFS / ext4<br/>write() · fsync()"]
        SIG["Signals<br/>SIGCHLD → waitpid()"]
    end

    subgraph USER["User space"]
        SUP["Supervisor / watchdog (E)<br/>1 thread · poll() loop<br/>forks + restarts all children"]

        subgraph LP["light_sensor process"]
            LT["main thread<br/>read → send → heartbeat"]
        end

        subgraph SP["soil_sensor process"]
            ST["main thread<br/>read → send → heartbeat"]
        end

        subgraph HUBP["hub process"]
            HT["main thread · poll() loop<br/>receive · validate · stamp seq+time · forward<br/>per-sensor last-seen → sensor_timeout"]
        end

        subgraph STOP["storage process (D)"]
            RX["rx thread<br/>append to batch"]
            BUF[["in-memory batch<br/>≤ 64 × 24 B records"]]
            FL["flush thread<br/>flush on N = 64 or T = 60 s"]
            REC["startup recovery<br/>scan tail · truncate bad CRC"]
        end

        subgraph DOWN["Planned · later milestone"]
            AN["Analysis engine"]
            UI["CLI / dashboard"]
        end
    end

    %% Data path
    LIGHT -->|"1 Hz, 2 B/read"| I2C
    SOIL -->|"1 per 60 s, 2 B/read"| I2C
    I2C -->|"1 Hz, 2 B/read"| LT
    I2C -->|"1 per 60 s, 2 B/read"| ST
    LT -->|"1 msg/s, 16 B"| HUBSOCK
    ST -->|"1 msg/60 s, 16 B"| HUBSOCK
    HUBSOCK -->|"≈1.02 msg/s"| HT
    HT -->|"≈1.02 records/s, ≈25 B/s"| STOSOCK
    STOSOCK -->|"≈1.02 records/s, ≈25 B/s"| RX
    RX -->|"≈1.02 records/s"| BUF
    BUF -->|"1 batch per 60 s, ≈1.5 KB"| FL
    FL -->|"1 batch + fsync per 60 s (or 64 records)"| VFS
    VFS -->|"≈1.5 KB/batch, ≈2.1 MB/day"| SD
    REC -.->|"once at startup, before accepting data"| VFS

    %% Downstream
    VFS -->|"reads new records every 60 s"| AN
    AN -->|"on request"| UI

    %% Control path
    LT -.->|"heartbeat 1 Hz"| SUPSOCK
    ST -.->|"heartbeat 1 Hz"| SUPSOCK
    HT -.->|"heartbeat 1 Hz"| SUPSOCK
    FL -.->|"heartbeat 1 Hz"| SUPSOCK
    SUPSOCK -.->|"heartbeat 1 Hz, timeout 5 s (4 msg/s total)"| SUP
    SIG -.->|"event: SIGCHLD"| SUP
    SUP -.->|"event: restart, backoff 1→2→4… max 30 s"| LP
    SUP -.->|"event: restart, backoff"| SP
    SUP -.->|"event: restart, backoff"| HUBP
    SUP -.->|"event: restart, backoff"| STOP
    SUP -.->|"event: restart, backoff"| DOWN

    style STOP fill:#e8f0fe,stroke:#3b6fd8
    style SUP fill:#fde8e8,stroke:#c0392b
```

## Mechanism mapping

**Mechanism D — Custom Storage Layer**

**Choice:** an append-only log of fixed-size, checksummed records, kept in one segment file per day on the microSD (ext4). We chose it over a ring buffer or FUSE because only the log's tail can ever be damaged, which makes the crash-consistency story the simplest to state and test. **Guarantee:** *a record is committed once fsync returns; a power cut loses at most one unsynced batch (≤ 60 s) and never leaves a corrupt record readable.*

| Requirement (Green Thumb) | Mechanism | Implementing component | Justification | Measured by / target |
|---|---|---|---|---|
| Plant care needs an unbroken multi-day history | Append-only log; daily segments; 30-day retention | storage process, flush thread | Soil moisture changes over hours and watering trends over days, so the value is in the history. About 2.1 MB/day means 30 days is about 63 MB, which fits easily | 72 h soak: records committed = records sent by the hub (0 sequence gaps) |
| Keep write overhead and SD wear low | Batching: flush at N = 64 records or T = 60 s; single buffer copied out under the lock, no lock held during I/O | rx thread + flush thread | Fsyncing every record would mean about 88k fsyncs/day; batching cuts that to about 1,440/day. Soil changes slowly, so a 60 s commit delay costs nothing | Compare fsync count, write latency and CPU at (N,T) = (1, 1 s), (64, 10 s) and (64, 60 s) |
| A precise, provable durability point | fsync per batch; directory fsync when a new segment is created | flush thread | Without fsync, "written" only means the data reached the page cache. The directory fsync stops a new day's file from vanishing | Every commit logged with count and duration; report fsync p50/p99 |
| Survive being unplugged mid-write | 24 B records with CRC32; startup scan truncates at the first bad record before accepting data | storage startup recovery | A household Pi has no UPS and gets unplugged routinely. Fixed-size records make recovery a short arithmetic scan | 20 real power pulls mid-batch: 0 corrupt records accepted, loss ≤ 1 batch (60 s), recovery < 1 s |
| No loss on a normal shutdown or restart | Flush and fsync on SIGTERM | flush thread | Supervisor restarts and user shutdowns shouldn't cost a batch | 20 SIGTERM restarts: 0 records lost |
| Never fail silently | Check every write/fsync; on any detected failure, keep the batch in memory and mark storage degraded; on the next cycle, truncate to the last committed offset, rewrite and fsync | flush thread | A silent hole would undermine the history claim, which is D's selling point | Inject write/fsync errors: 100% logged as `storage_write_error` / `fsync_error` + `storage_degraded`, 0 silent losses |
| Preserve history through a storage outage | 1 MB queue (about 11 h); `storage_backlog` at 50/75/90%; when full, an explicit degraded state that refuses and counts new readings, then writes a gap record on recovery | storage process | Better a marked gap than a silent one. Storage keeps reading its socket, so the hub and sensors never block and E's heartbeats are unaffected | Outage longer than the queue (with a small test cap): 0 accepted readings lost, and the gap record's count equals the refused count |
| Restarts must not corrupt the log (D meets E) | Recovery runs before `storage.sock` accepts data | storage startup recovery + supervisor | The supervisor can kill storage at any instant, so a kill is just a power cut without the reboot | 50 random `kill -9`s on storage: 0 corrupt records |

## Failure-mode table

Log format for every component: `timestamp level component event key=value`. Rule: whichever component refuses a reading records the gap (start, end, count), so no reading is ever lost silently.

| Component | Failure | Detection | Response | Log |
|---|---|---|---|---|
| Sensor (I2C) | Read fails (NACK, EIO, unplugged) | `read`/`ioctl` returns -1 | Retry 3× at 10 ms, then skip the sample; after 10 consecutive failures, mark offline and retry every 5 s | `i2c_read_error`, `sensor_offline`, `sensor_online` |
| Sensor → hub | Hub down or restarting; local ring overflows | `send`/`connect` returns EPIPE, ECONNREFUSED or ENOENT; ring full | Buffer up to 64 readings, reconnect with 100 ms → 2 s backoff, then resend; on overflow, refuse and count, then send a gap record on reconnect | `ipc_disconnect`, `ipc_reconnect`, `ipc_overflow` |
| Hub | Malformed, lost or out-of-order message | Length/magic/version check; gap in the sensor-side counter | Discard a malformed message and close the connection so the sensor resyncs; report counter gaps | `ipc_bad_message`, `sensor_seq_gap` |
| Hub | Sensor goes quiet | Last-seen timer: 3 s for light, 180 s for soil | Mark the sensor stale and keep running on the other sensor | `sensor_timeout`, `sensor_resumed` |
| Hub | Out-of-range reading | Range check (light 0–65535 lux; soil within the seesaw's raw range) | Reject and count; invalid data is not stored | `reading_invalid` |
| Hub | Process crashes or hangs | SIGCHLD, or no heartbeat for 5 s | Supervisor restarts it with backoff (SIGKILL first if hung); sensor rings cover the downtime | `child_exit` / `heartbeat_timeout`, `hub_start` |
| Hub → storage | Storage restarting, recovering or slow | `send` returns EPIPE, ECONNREFUSED or EAGAIN | Hold up to 1024 records (about 17 min), reconnect, and drain in order | `storage_unreachable`, `storage_reconnected` |
| Hub → storage | Hub queue full | Queue depth | Refuse and count new readings; send one gap record to storage before draining | `hub_queue_full` |
| Storage | `write()` or `fsync()` fails | Return value check | Keep the batch in memory and mark degraded; on the next cycle, truncate the log back to the last committed offset, then rewrite the batch and fsync, so no duplicate or partial records reach the log | `storage_write_error` / `fsync_error`, `storage_degraded` |
| Storage | Disk full, or backlog fills during an outage | ENOSPC / `statvfs`; queue depth (1 MB, about 11 h) | Log the backlog at 50/75/90%; never delete history early; when full, refuse and count new readings and write a gap record on recovery | `storage_disk_full`, `storage_backlog`, `storage_degraded`, `storage_recovered` |
| Storage | Corrupt tail or bad segment header after a crash | Startup scan: bad magic, CRC or header, backwards seq, partial record | Truncate at the first bad record (or rename a bad segment to `.corrupt`) and fsync, then accept data | `recovery_event`, `segment_corrupt` |
| Storage | Process crashes or hangs | SIGCHLD, or no heartbeat for 5 s | Supervisor restarts it; recovery runs before the socket accepts data | `child_exit` / `heartbeat_timeout`, `recovery_event` |
| Power | Power cut mid-write | No clean-shutdown marker at boot | Recovery scan before accepting data. A record is committed once fsync returns; a power cut loses at most one unsynced batch (≤ 60 s), plus any backlog held in memory during a storage outage, and never leaves a corrupt record readable | `unclean_shutdown`, `recovery_event` |
| System clock | Wrong time after a reboot (no real-time clock) | NTP sync status from `adjtimex` | Store readings anyway (seq keeps their order) and log the unsynced period | `clock_unsynced`, `clock_synced` |

## Storage and data

All sensor data is written by one component, the **storage process** (Mechanism D). No other process writes to the log. See the *Architecture Diagram* (storage subgraph) and *Mechanism mapping → Mechanism D*.

### What is stored

The log holds two kinds of fixed-size record:

- **DATA record:** one accepted reading from one sensor. It holds the sensor ID, the hub-assigned sequence number, a timestamp and the sensor's **raw count**:
  - Light (BH1750): raw 16-bit count. Lux = count / 1.2, converted when the data is read.
  - Soil (STEMMA seesaw): raw capacitance count, roughly 200–2000.

  Light and soil are stored as separate records, not combined into one row. Raw counts are stored rather than converted units, so calibration can change later without rewriting history.
- **GAP record:** an explicit marker that readings were refused. It holds the affected sensor, the gap's start time and the number of refused readings. The component that refuses the readings writes the gap record (sensor ring, hub queue or storage queue). See the rule above the *Failure-mode table*.

Readings that fail validation (out of range, or a malformed message) are rejected by the hub and are **not** stored. See the *Failure-mode table → Hub* rows.

The timestamp is applied by the hub when it receives the reading. If the Pi booted without network time, the timestamps may be wrong until NTP syncs. Ordering always comes from `seq`. See the *Failure-mode table → System clock*.

### At what rate

| Stream | Rate | Records/day |
|---|---|---|
| Light | 1 per second | 86,400 |
| Soil | 1 per 60 s | 1,440 |
| **Total into storage** | **≈1.02 records/s (≈25 B/s)** | **87,840 ≈ 2.1 MB/day** |

On the wire, each reading is a 16 B socket message (sensor → hub → storage). On disk, each reading is one 24 B record. Gap records only appear during failures and add negligible volume. See the *Architecture Diagram* data path labels.

### In what format

The log is an **append-only binary log** of fixed 24 B records, packed and little-endian.

**Segment header** (16 B, written once at the start of each file):

| Field | Size | Value |
|---|---|---|
| magic | 4 B | `"GTLG"` |
| version | 2 B | 1 |
| record_size | 2 B | 24 |
| created_at | 8 B | µs since the Unix epoch |

**Record** (24 B):

| Field | Size | DATA record | GAP record |
|---|---|---|---|
| magic | 1 B | `0xA7` | `0xA7` |
| record_type | 1 B | `0x01` | `0x02` |
| version | 1 B | 1 | 1 |
| sensor_id | 1 B | 1 = light, 2 = soil | affected sensor (0 = all) |
| seq | 4 B | hub sequence number | see below |
| timestamp | 8 B | hub receive time, µs since epoch | gap start, µs since epoch |
| value | 4 B | raw sensor count (u32) | refused-reading count (u32) |
| crc32 | 4 B | CRC-32 over bytes 0–19 | CRC-32 over bytes 0–19 |

- A gap ends at the timestamp of the next DATA record for the same sensor, so it needs no separate end field.
- Every record has a `seq`, and `seq` is strictly increasing across the log:
  - Gaps created by a sensor or by the hub receive a hub-assigned `seq`.
  - Gaps created by storage use the `seq` of the first refused reading.
- Any record with an unknown `record_type` is treated as corrupt.

### How records are organized

- There is one segment file per UTC day: `/var/lib/plantpi/log-YYYYMMDD.bin`.
- Records are appended in `seq` order. Only the current day's file is open for writing, so only its tail can ever need recovery.
- When a new segment is created, storage fsyncs the directory as well, so the new file cannot disappear after a power cut.

### With what retention

- Segments are kept for **30 days**, about 63 MB. When a new day's segment is created, segments older than 30 days are deleted.
- **Nothing inside the 30-day window is ever deleted to make room.** If the card fills before 30 days:
  - Storage logs `storage_disk_full`.
  - It refuses and counts new readings.
  - It writes a GAP record once space is available again.
- A 64 KB reserve (checked with `statvfs`) is always kept free, so a GAP record can always be written.

This follows the project's policy: protect existing history, and make any unavoidable loss explicit. See *Failure-mode table → Storage (disk full)*.

### How writes are batched and when data is flushed

- The **rx thread** reads records from `storage.sock` and appends them to a single in-memory batch buffer (≤ 64 × 24 B) under a mutex.
- The **flush thread** commits a batch when either of these comes first:
  - **N = 64 records**, or
  - **T = 60 s** since the last commit. At about 1.02 records/s, T is normally the trigger, which gives about 61 records (≈1.5 KB) per batch and about 1,440 fsyncs/day.
- **Commit sequence:**
  1. Lock the mutex, copy the batch out and clear the buffer.
  2. Unlock. The lock is never held during I/O.
  3. `write()` the batch.
  4. `fsync()` the segment.
  5. Record the new committed offset.
- **On SIGTERM**, the flush thread commits whatever is buffered, then writes a clean-shutdown marker.
- **A record is committed once `fsync()` returns.** Before then, it may be lost but is never reported as stored.
- **If `write()` or `fsync()` fails:**
  - The batch stays in memory and storage is marked degraded immediately.
  - On the next cycle, storage truncates the file back to the last committed offset, then rewrites and fsyncs the batch, so no duplicate or partial records reach the log.
  - While the disk is unavailable, records keep queueing, up to 1 MB (about 11 h). Storage logs `storage_backlog` at 50%, 75% and 90%.
  - When the queue is full, storage refuses and counts new readings, and writes a GAP record on recovery.

See *Mechanism D* (batching, durability point and outage rows) and *Failure-mode table → Storage*.

### What happens when the power dies mid-write

**Guarantee:** a power cut loses at most one unsynced batch (≤ 60 s), plus any backlog held in memory during a storage outage. It never leaves a corrupt record readable.

On every start, **recovery runs before `storage.sock` accepts any data**. This is the same path after a power cut and after a supervisor restart or `kill -9`.

1. If the clean-shutdown marker is missing, log `unclean_shutdown`, then remove the marker.
2. Check the current segment's header. If it is invalid, rename the file to `.corrupt`, log `segment_corrupt`, and start a fresh segment.
3. Scan the records from the start of the segment. Stop at the **first** record that has any of these problems:
   - bad magic, unknown `record_type` or wrong version
   - CRC mismatch
   - `seq` that does not increase
   - a partial (fewer than 24 B) trailing record
4. `ftruncate()` the file at that offset, `fsync()` it, and log `recovery_event` with the bytes and records removed.
5. Open `storage.sock` and resume normal operation.

Records are fixed-size, so the scan is simple offset arithmetic. The target is under 1 s for a full day's segment.

See *Mechanism D → "Survive being unplugged" and "Restarts must not corrupt the log"*, and *Failure-mode table → Storage (corrupt tail)* and *Power*.

## Constraints and substitutions. What the ideal build would use, what you’re actually using, and what the gap costs: sensors downgraded, data synthesized or replayed, claims narrowed (Section 5). A design document with nothing to report here has usually not met its hardware yet.
Briefly document:
- [ ] Raspberry Pi CPU/RAM limitations
- [ ] SD/storage limitations
- [ ] IPC overhead
- [ ] Budget limitations
- [ ] Power-loss considerations

## Evaluation plan. The measurements you will take (Section 9), each with its method and its target. Numbers you commit to now are twice as credible when you hit them later. And instructive either way.
Write down what you'll eventually measure:
- [ ] IPC latency
- [ ] Sensor-to-storage latency
- [ ] Storage write throughput
- [ ] CPU usage
- [ ] Memory usage
- [ ] Data loss/corruption
- [ ] Recovery time after failure
- [ ] Behavior under sustained sampling

## Ownership map. Which partner owns which subsystems (Section 10.1). Ownership means first authorship and answerability at the defense, not exclusivity.
