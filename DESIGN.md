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
| Never fail silently | Check every write/fsync; keep the batch and retry; degraded after 3 consecutive failures | flush thread | A silent hole would undermine the history claim, which is D's selling point | Inject write/fsync errors: 100% logged as `storage_write_error`, 0 silent losses |
| Preserve history through a storage outage | 1 MB queue (about 11 h); `storage_backlog` at 50/75/90%; when full, an explicit degraded state that refuses and counts new readings, then writes a gap record on recovery | storage process | Better a marked gap than a silent one. Storage keeps reading its socket, so the hub and sensors never block and E's heartbeats are unaffected | Outage longer than the queue (with a small test cap): 0 accepted readings lost, and the gap record's count equals the refused count |
| Restarts must not corrupt the log (D meets E) | Recovery runs before `storage.sock` accepts data | storage startup recovery + supervisor | The supervisor can kill storage at any instant, so a kill is just a power cut without the reboot | 50 random `kill -9`s on storage: 0 corrupt records |

## Failure-mode table. For each component: how it can fail, how the failure is detected, what the system does about it, and what the log will show. The soak test (Section 3.4) will grade this table’s honesty.

| Component | Failure | Detection | Response | Log |
|---|---|---|---|---|
| Sensor → IPC | Message never arrives | Timeout | Log/drop/retry | `sensor_timeout` |
| IPC | Connection failure | Error/timeout | Reconnect/restart | `ipc_failure` |
| Hub | Process dies | Supervisor | Restart | `hub_restart` |
| Storage | Write fails | Return/error check | Log/retry/degrade | `storage_write_error` |
| Storage | Partial record | Record validation | Discard/recover | `corrupt_record` |
| Power | Mid-write failure | Record validation | Recover log | `recovery_event` |
| Supervisor | Child process dies | Process monitoring | Restart | `child_restart` |

**• Storage and data. What is stored, at what rate, in what format, with what retention, and what happens to it when the power dies mid-write.**
What exactly is stored?
[ ] Soil + light + timestamp?
[ ] What is the sampling rate?
[ ] What is the storage format?
[ ] How are records organized?
[ ] How long are they retained?
[ ] How are writes batched?
[ ] When is data flushed?
[ ] How do you recover from a mid-write failure?

**• Constraints and substitutions. What the ideal build would use, what you’re actually using, and what the gap costs: sensors downgraded, data synthesized or replayed, claims narrowed (Section 5). A design document with nothing to report here has usually not met its hardware yet.**
Briefly document:
- [ ] Raspberry Pi CPU/RAM limitations
- [ ] SD/storage limitations
- [ ] IPC overhead
- [ ] Budget limitations
- [ ] Power-loss considerations

**• Evaluation plan. The measurements you will take (Section 9), each with its method and its target. Numbers you commit to now are twice as credible when you hit them later. And instructive either way.**
Write down what you'll eventually measure:
- [ ] IPC latency
- [ ] Sensor-to-storage latency
- [ ] Storage write throughput
- [ ] CPU usage
- [ ] Memory usage
- [ ] Data loss/corruption
- [ ] Recovery time after failure
- [ ] Behavior under sustained sampling

**• Ownership map. Which partner owns which subsystems (Section 10.1). Ownership means first authorship and answerability at the defense, not exclusivity.**
Lily (Heron-i) owns the: sunlight sensor, IPC/data pipeline, Hub/aggregator, storage, mechanism D, recovery/supervision, and pipeline evaluation
