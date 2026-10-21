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

**• Mechanism mapping. For each chosen menu item: which component implements it, and a justification from the user’s requirements (“the vibration analysis is meaningless above 2 ms of sampling jitter, hence SCHED_FIFO”).**

### Mechanism D — Custom Storage Layer

| Requirement / Problem | Mechanism | Implementing Component | How It Works | How We Measure It |
|---|---|---|---|---|
| Sensor data needs to remain available for later plant-care analysis | Custom storage layer | Storage subsystem | Stores sensor readings in a persistent custom data format | Verify all generated samples are recoverable from storage |
| Green Thumb continuously produces sensor data | Custom storage layer | Storage subsystem | Collects sensor records and writes them to storage in controlled batches | Measure sustained write throughput and storage rate |
| Frequent individual writes could add unnecessary overhead | Custom storage layer | Storage subsystem | Batches multiple sensor records before committing them to storage | Compare write behavior/latency with different batch sizes |
| Sensor data should survive normal program termination | Custom storage layer | Storage subsystem | Explicitly flushes/commits buffered data to persistent storage | Restart system and verify previously stored records remain intact |
| Mid-write power failure could leave incomplete/corrupt data | Custom storage layer | Storage/recovery component | Detects incomplete records when storage is reopened and recovers or discards them | Inject interrupted writes and measure successful recovery/data loss |
| The system should detect storage failures rather than silently losing data | Custom storage layer | Storage subsystem | Checks write/flush errors and records failures in the system log | Inject storage/write failures and verify detection + logging |
1. Mechanism D — make the decision

My task:
- [ ] Decide exactly what your custom storage layer is.
- [ ] Decide what data structure/file format it uses.
- [ ] Decide how writes happen.
- [ ] Decide how batching works.
- [ ] Decide when fsync() happens.
- [ ] Decide what happens if power fails during a write.
- [ ] Write why this mechanism is useful for Green Thumb, not just how it works.

**• Failure-mode table. For each component: how it can fail, how the failure is detected, what the system does about it, and what the log will show. The soak test (Section 3.4) will grade this table’s honesty.**

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
