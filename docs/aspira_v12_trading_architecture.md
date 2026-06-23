# V12 Ultra-Lightweight Low-Latency Trading System Architecture

## 1. Design Goals

V12 is a **production-oriented evolution of V11**, targeting:

- Ultra-low latency (sub-millisecond, optimized path < 10µs intra-process)
- Deterministic execution (strict ordering via sequencer)
- Zero-copy message passing
- NUMA-aware execution
- Lock-free or single-writer architecture
- Clean separation of concerns (broker-grade isolation)
- High reliability (WAL + replay recovery)

---

## 2. High-Level Architecture

```
                +----------------------+
Market Data --> |  Market Gateway      |
                +----------+-----------+
                           |
                           v
                +----------------------+
Order Entry --> |  Order Gateway       |
                +----------+-----------+
                           |
                           v
                +----------------------+
                |  Global Sequencer    |
                | (Single Source Truth)|
                +----------+-----------+
                           |
        +------------------+------------------+
        |                  |                  |
        v                  v                  v
+--------------+  +--------------+  +--------------+
| Risk Engine  |  | Risk Engine  |  | Risk Engine  |
| (Fast Path)  |  | (Shard N)    |  | (Shard N+1)  |
+------+-------+  +------+-------+  +------+-------+
       |                 |                  |
       +-----------------+------------------+
                           |
                           v
                +----------------------+
                | Matching Engine Pool |
                | (Shard-based Books)  |
                +----------+-----------+
                           |
                           v
                +----------------------+
                | Clearing & Settlement|
                +----------+-----------+
                           |
                           v
                +----------------------+
                | Journal / WAL        |
                | Recovery System      |
                +----------------------+
```

---

## 3. Core Principles

### 3.1 Single Writer Principle

Each critical component follows:

- One thread owns one state
- No shared mutable state
- Communication only via ring buffers

---

### 3.2 Deterministic Sequencing

All orders must pass through:

```
Global Sequencer → assigns monotonically increasing sequence ID
```

Guarantees:

- Price-time priority
- Replay consistency
- Audit correctness

---

### 3.3 Zero-Copy Messaging

- Pre-allocated buffers
- MPSC ring buffers
- No malloc in hot path

---

## 4. Core Modules

---

## 4.1 Market Gateway

Responsibilities:

- Parse market feeds
- Normalize tick data
- Push into internal bus

Constraints:

- Lock-free ingestion
- Drop policy for overload (configurable)

---

## 4.2 Order Gateway

Responsibilities:

- Accept client orders
- Validate schema
- Forward to sequencer

Optimizations:

- batch enqueue
- pre-validation cache

---

## 4.3 Global Sequencer (CRITICAL)

The heart of system.

### Responsibilities:

- Assign global sequence ID
- Ensure deterministic ordering
- Dispatch to downstream engines

### Implementation:

- Single-threaded
- CPU pinned core
- Pre-allocated queue

### Guarantee:

```
Seq(n) < Seq(n+1)
```

---

## 4.4 Risk Engine

Two-stage design:

### Fast Path Risk

- limit check
- margin check
- position sanity

### Deep Risk (async)

- exposure recalculation
- portfolio stress test
- audit validation

---

## 4.5 Matching Engine (Shard-Based)

### Design:

- Each symbol group → one engine shard
- Single writer per shard
- No locking

### Data structure:

- Price levels: skip list or array buckets
- Order queue: FIFO per price level

### Rule:

```
Only sequencer can assign order to shard
```

---

## 4.6 Clearing Engine

- Netting
- Position updates
- Trade confirmation generation

Optimized for:

- batch processing
- memory locality

---

## 4.7 Journal / WAL

Critical subsystem.

### Guarantees:

- Write-before-execute
- Append-only log
- Crash recovery replay

### Format:

```
[SEQ_ID][ORDER_ID][EVENT_TYPE][PAYLOAD]
```

---

## 4.8 Recovery System

- Replay WAL
- Rebuild order books
- Validate consistency

Modes:

- cold start replay
- partial recovery
- audit reconstruction

---

## 5. Memory Architecture

### 5.1 Memory Pools

- fixed-size slab allocator
- per-thread local cache
- global fallback pool

### 5.2 HugePage Support

- reduces TLB misses
- improves cache predictability

---

## 6. Threading Model

```
Core 0: Sequencer
Core 1: Market Gateway
Core 2: Order Gateway
Core 3-6: Matching Shards
Core 7: Risk Fast Path
Core 8: Clearing
Core 9: Journal Writer
```

---

## 7. Queue Design

- MPSC ring buffer
- power-of-two size
- cache-line aligned
- memory fencing via atomic

---

## 8. Performance Targets

| Metric | Target |
|--------|--------|
| Order latency | < 50µs |
| Matching latency | < 10µs |
| Seq dispatch | < 1µs |
| Memory alloc | 0 in hot path |

---

## 9. Fault Tolerance

- WAL replay
- checkpoint snapshots
- engine shard isolation

---

## 10. Isolation Model

- CPU isolation (taskset / isolcpus)
- NUMA binding
- per-thread memory locality

---

## 11. Key Improvements over V11

- Removed shared mutable matching state
- Introduced global sequencer (deterministic)
- Clean shard ownership model
- Strong WAL ordering guarantee
- Clear separation of fast/slow risk path
- Production-grade memory discipline

---

## 12. Future Extensions

- FPGA feed handler
- kernel bypass (DPDK / AF_XDP)
- distributed matching cluster
- cross-venue arbitrage engine

---

## END OF V12 ARCHITECTURE
