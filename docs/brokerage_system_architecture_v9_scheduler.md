# V9 Isolated Brokerage & Trading System Architecture
## (With Integrated Process & Thread Scheduler Design)

---

# 0. Design Goal

V9 extends V8 by introducing a **deterministic scheduling subsystem** that governs:

- Process scheduling (trading services isolation runtime)
- Thread scheduling (hot-path deterministic execution)
- CPU core binding & NUMA-aware placement
- Latency guarantees for matching engine
- Global resource arbitration across shards

The goal is:

> Make execution not only isolated, but also *predictably scheduled*

---

# 1. Core Additions in V9

Compared to V8, V9 introduces:

- Process Scheduler (Cluster-level control plane)
- Thread Scheduler (Per-process execution engine)
- CPU Core Allocation Engine
- Deterministic Hot-Path Execution Policy
- Latency Budget Enforcement System
- Real-time Scheduling Telemetry

---

# 2. System Architecture (V9 Extended)

```
Client Layer
   ↓
API Gateway Cluster
   ↓
Service Layer (OMS / Risk / Account / KYC / Funding)
   ↓
TRADING CORE ZONE
   ├── Process Scheduler (NEW)
   ├── Thread Scheduler (NEW)
   ├── Sequencer
   ├── Matching Engine Shards
   └── Market Data Engine
   ↓
POST TRADE ZONE
   ├── Ledger
   ├── Position
   ├── Clearing
   └── Settlement
   ↓
COMPLIANCE ZONE
   ├── AML
   ├── Surveillance
   ├── Reporting
   └── Audit
```

---

# 3. Scheduling Philosophy

## 3.1 Hard Determinism Rule

> Scheduling must never break determinism in trading core

Rules:

- Matching engine execution order is NOT preemptible
- No thread migration in hot path
- No dynamic priority inversion in trading core
- Scheduling decisions must be reproducible

---

## 3.2 Scheduling Layers

```
Layer 1: Process Placement Scheduler
Layer 2: CPU Core Allocator
Layer 3: Thread Scheduler (in-process)
Layer 4: Hot-path Execution Guard
```

---

# 4. Process Scheduler (Cluster Level)

## 4.1 Responsibilities

- Assign services to physical CPU cores
- Ensure NUMA locality
- Prevent CPU oversubscription in hot zones
- Maintain isolation between shards
- Enforce latency budgets per process

---

## 4.2 Process Classes

### HOT PATH PROCESSES

- broker-matching-engine-shard-N
- broker-sequencer
- broker-marketdata-engine

### WARM PATH PROCESSES

- OMS
- Risk Engine
- EMS

### COLD PATH PROCESSES

- KYC
- Reporting
- Admin
- Compliance

---

## 4.3 Allocation Model

```
Core Group A (HOT)
- Matching Engine Shard 1
- Sequencer
- Market Data Core

Core Group B (WARM)
- OMS
- Risk Engine

Core Group C (COLD)
- KYC / Reporting / Admin
```

---

## 4.4 Rules

- No hot + cold mixing on same NUMA node
- No CPU overcommit in HOT group
- Fixed binding at boot time (no runtime migration)

---

# 5. Thread Scheduler (In-Process Engine)

## 5.1 Design Principle

> Thread scheduling inside trading core must be deterministic and static

No:
- work stealing
- dynamic thread pool resizing
- OS-level migration in hot path

---

## 5.2 Matching Engine Thread Model

Each shard:

```
Thread 0: IO Ingress
Thread 1: Sequencer Input
Thread 2: MATCHING CORE (SINGLE THREAD, HOT PATH)
Thread 3: Market Data Publisher
```

---

## 5.3 Execution Guarantee

- Only one thread modifies order book
- No locks
- No context switching inside matching loop

---

## 5.4 Thread Affinity Binding

```text
pthread_setaffinity_np()
```

Rules:

- each thread pinned to fixed core
- no migration allowed
- OS scheduler interference minimized

---

# 6. CPU Core Allocation Engine

## 6.1 Core Map

Example:

```
CPU 0  → Sequencer
CPU 1  → Matching Shard A
CPU 2  → Matching Shard B
CPU 3  → Market Data Engine
CPU 4-7 → Risk Engine pool
CPU 8-15 → OMS / EMS
CPU 16-31 → Cold services
```

---

## 6.2 NUMA Awareness

Each shard must:

- allocate memory on local NUMA node
- avoid cross-node cache misses
- pin IO buffers per node

---

# 7. Latency Budget System

## 7.1 Budget Definition

Each request has a fixed budget:

| Stage | Budget |
|------|--------|
| API ingress | 200μs |
| Risk check | 50μs |
| Sequencing | 5μs |
| Matching | 10μs |
| Execution publish | 100μs |

---

## 7.2 Budget Enforcement

If exceeded:

- degrade service
- reject order
- switch to safe mode

---

# 8. Deterministic Execution Model

## 8.1 Rule

> Given same input events, system must produce identical output

Ensured by:

- fixed scheduling
- single-thread matching core
- event-sourced state
- deterministic sequencer

---

# 9. Scheduler Control Plane

## 9.1 Scheduler Service

```
broker-scheduler-control-plane
```

Responsibilities:

- cluster CPU topology detection
- process placement
- runtime health monitoring
- core rebalancing (cold path only)

---

## 9.2 Scheduling Metadata

```json
{
  "process": "matching-engine-shard-1",
  "cpu_affinity": [1],
  "numa_node": 0,
  "priority": "REALTIME",
  "latency_budget_us": 10
}
```

---

# 10. Failure Handling in Scheduler

## 10.1 Process Failure

- restart on same core
- restore from snapshot + event log

## 10.2 Core Failure

- migrate shard to standby core
- replay event log

## 10.3 Scheduler Failure

- fallback to static placement config
- freeze scheduling changes

---

# 11. Integration with V8 Architecture

## 11.1 What Changes

V8:
- static isolation only

V9:
- isolation + controlled scheduling

---

## 11.2 New Dependency Chain

```
Scheduler → Process Placement
Scheduler → Thread Binding
Scheduler → CPU Allocation
Matching Engine → Scheduler Policy
Risk Engine → Scheduler Budget
```

---

# 12. Performance Impact

## Improvements

- reduced cache miss rate
- improved NUMA locality
- reduced context switching
- deterministic latency distribution

---

# 13. Key Constraints

> Scheduling system must NEVER be in hot path of matching execution

Scheduler only:

- configures
- monitors
- enforces boundaries

NOT:

- participates in order processing

---

# 14. Final Architecture Summary

V9 introduces a critical missing piece in V8:

> A deterministic scheduling system for both processes and threads

This ensures:

- predictable latency
- strict isolation
- controlled CPU usage
- NUMA-aware performance
- production-grade stability under load

---

# Key Principle

> "Not only isolate the system, but also precisely control how it executes in time and space."
