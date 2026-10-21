# CS370: Term Project
For Colorado State University's Operating Systems course, we were tasked with creating a term project that would address a real-world problem and present a solution that isn't easily solved with an application. This project suggests utilizing a low cost, single-board computer (Raspberry Pi) with a minimum of two sensors.

## Our Project: The Plant Owner's Capable Assistant (in-progress)
For our project, we are creating a mini houseplant assistant that enables a user to give their plants the best care possible by measuring soil-moisture and light exposure. We believe this project is not easily solved by an app, and that current similar products on the market, have shown common gaps in their capabilities. Thus, we want to create an effective solution that will help bridge the communication gap between plants and humans.

```mermaid
flowchart TB

    %% =========================
    %% USER SPACE
    %% =========================

    SAMPLER["sampler daemon<br/>SCHED, FIFO (F)<br/>no-drop ring"]

    STORAGE["storage layer (D)<br/>ring / append log, mmap / mq"]

    ANALYSIS["analysis engine<br/>your analysis, DSP,<br/>state machines"]

    INTERFACE["interface<br/>CLI / LAN<br/>dashboard"]

    %% =========================
    %% KERNEL / USER BOUNDARY
    %% =========================

    BOUNDARY["kernel / user boundary"]

    %% =========================
    %% KERNEL SPACE
    %% =========================

    KERNEL["kernel space: I2C / GPIO subsystems<br/>your character driver (A)"]

    IRQ["IRQ handling (B)"]

    SUPERVISOR["supervisor /<br/>watchdog (E)"]

    %% =========================
    %% HARDWARE
    %% =========================

    SENSOR_A["Sensor A<br/>I2C, 10 Hz"]

    SENSOR_B["Sensor B<br/>GPIO, event-driven"]

    ACTUATOR["Actuator<br/>(optional)"]

    %% =========================
    %% DATA FLOWS
    %% =========================

    SAMPLER -->|"shm ring"| STORAGE
    STORAGE -->|"mmap / mq"| ANALYSIS
    ANALYSIS -->|"UDS"| INTERFACE

    %% =========================
    %% KERNEL / HARDWARE FLOWS
    %% =========================

    SENSOR_A --> KERNEL
    SENSOR_B --> KERNEL

    KERNEL -->|"IRQ"| IRQ
    IRQ -.->|"sensor events"| SAMPLER

    ANALYSIS -.->|"control"| ACTUATOR

    %% =========================
    %% SUPERVISION
    %% =========================

    SUPERVISOR -.->|"restart on hang or crash"| SAMPLER
    SUPERVISOR -.->|"restart on hang or crash"| STORAGE
    SUPERVISOR -.->|"restart on hang or crash"| ANALYSIS

    %% =========================
    %% STYLING
    %% =========================

    classDef process fill:#eaf2f8,stroke:#1f4e79,stroke-width:1px;
    classDef kernel fill:#333333,color:#ffffff,stroke:#222222,stroke-width:1px;
    classDef sensor fill:#f2f2f2,stroke:#888888,stroke-width:1px;
    classDef supervisor fill:#fff0e6,stroke:#c55a11,stroke-width:1px;
    classDef boundary fill:#ffffff,color:#555555,stroke:#aaaaaa,stroke-dasharray:5 5;

    class SAMPLER,STORAGE,ANALYSIS,INTERFACE process;
    class KERNEL,IRQ kernel;
    class SENSOR_A,SENSOR_B,ACTUATOR sensor;
    class SUPERVISOR supervisor;
    class BOUNDARY boundary;
```
