# Exchange-Grade Isolated Trading System (V6)

A **fully isolated, fault-domain-separated, production-grade electronic trading platform** targeting Binance/NASDAQ-level performance. Built with **C11** and **C++20**, featuring process-level isolation, event sourcing, global sequencing, NUMA-aware tiered memory, a sharded deterministic matching engine, and a full clearing & settlement layer — capable of **6.65+ million orders per second** through the full pipeline.

> Based on `docs/trading_system_architecture_v6.md`

---

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [Performance](#performance)
- [System Components](#system-components)
- [Data Flow](#data-flow)
- [Project Structure](#project-structure)
- [Build & Run](#build--run)
- [Configuration](#configuration)
- [Hot-Path Design](#hot-path-design)
- [Threading Model](#threading-model)
- [Matching Engine](#matching-engine)
- [Sequencer](#sequencer)
- [Risk Control](#risk-control)
- [Memory Management](#memory-management)
- [Persistence & Recovery](#persistence--recovery)
- [Market Data](#market-data)
- [Benchmarking](#benchmarking)
- [Design Goals](#design-goals)
- [License](#license)

---

## Architecture Overview

```
                         ┌──────────────────────┐
                         │      CLIENTS          │
                         │  (TCP / Binary Proto) │
                         └──────────┬───────────┘
                                    │
                     ┌──────────────▼──────────────┐
                     │       API GATEWAY            │
                     │  epoll ET · rate limiting    │
                     │  non-blocking I/O · FIX-lite │
                     └──────────────┬──────────────┘
                                    │  MPSC Queue
                     ┌──────────────▼──────────────┐
                     │   ORDER GATE (V5 NEW)        │
                     │  backpressure · shaping      │
                     │  early validation · norm.    │
                     └──────────────┬──────────────┘
                                    │  MPSC Queue
                     ┌──────────────▼──────────────┐
                     │  ORDER MANAGEMENT (OMS)      │
                     │  validation · local seq IDs  │
                     │  order lifecycle state mach. │
                     └──────────────┬──────────────┘
                                    │  MPSC Queue
                     ┌──────────────▼──────────────┐
                     │     RISK ENGINE (×N)         │
                     │  position limits · exposure  │
                     │  circuit breaker · kill sw.  │
                     └──────────────┬──────────────┘
                                    │  MPSC Queue
                     ┌──────────────▼──────────────┐
                     │       SEQUENCER              │
                     │  global deterministic seq ID │
                     │  symbol-hash → shard routing │
                     └──────────────┬──────────────┘
                                    │  MPSC Queue × N shards
                     ┌──────────────▼──────────────┐
                     │   MATCHING ENGINE (×N)       │
                     │  skip-list order books       │
                     │  price-time priority · FIFO  │
                     │  LIMIT / MARKET / IOC / FOK  │
                     └──────┬────────┬──────┬───────┘
                            │        │      │
               ┌────────────▼──┐ ┌───▼───┐ │
               │  MARKET DATA  │ │ JRNL  │ │
               └───────────────┘ └───────┘ │
                                    ┌──────▼──────────┐
                                    │   EVENT BUS      │
                                    │ (V5: event src.) │
                                    └──────┬──────────┘
                                           │
                              ┌────────────┼────────────┐
                              ▼            ▼            ▼
                         ┌─────────┐ ┌──────────┐ ┌─────────┐
                         │CLEARING │ │PERSIST   │ │ AUDIT   │
                         │(V5 NEW) │ │(journal) │ │(future) │
                         └─────────┘ └──────────┘ └─────────┘
```

## Performance

Benchmarked on **Intel i9-12900H (20 cores)**, 16 GB RAM, Linux 6.17, GCC 13.3:

| Metric | V6 Result | Target |
|--------|-----------|--------|
| **Throughput** | **6.65M orders/sec** (full pipeline) | ≥ 5M |
| **Process Isolation** | Fork-per-shard + shared memory IPC | Fault-domain separation |
| **Memory Tiering** | HOT/WARM/COLD 3-tier allocation | Latency-classified |
| **Sequencer** | 111K global seq IDs, 0 drops | Deterministic |
| **Event Bus** | 104K events published, 37K delivered | Event sourced |
| **Clearing** | 37K trades settled, **$6.3B notional**, 74K ledger entries | Full settlement |
| **Order Gate** | 99.5% pass rate, backpressure active | Traffic protection |
| **Matching shards** | 4 (configurable, process-isolated) | Horizontal |
| **Hot Standby** | Journal replay infrastructure ready | Fault recovery |
| **Memory pool** | 4 GB NUMA-aware, tiered (HOT/WARM/COLD) | HugePages |

> **Note:** V6 introduces **process-level isolation** — each matching shard can run as an independent OS process with shared-memory IPC. The 9-stage pipeline (GW→OrderGate→OMS→Risk→Sequencer→Match×4→MD→EventBus→Clearing) operates concurrently via lock-free MPSC/SPSC queues. Memory is classified into HOT/WARM/COLD tiers with strict access rules. Branchless optimizations in the shard hot path deliver 14% throughput improvement over V5.

## System Components

### 1. API Gateway (`src/net/gateway.c`)
- TCP server with **epoll** edge-triggered I/O
- Non-blocking sockets with `SO_REUSEPORT`, `TCP_NODELAY`
- Simple binary protocol: `[4B length][1B type][N-byte payload]`
- FIX-lite payload format: `key=value|key=value|...`
- Token-bucket rate limiting per connection
- Up to 1,024 concurrent connections

### 2. Order Gate (`src/core/order_gate.c`) — V5 NEW
- Sits between Gateway and OMS to protect the core pipeline
- **Backpressure control**: monitors downstream queue depth
- **Request shaping**: smooths bursty traffic with μs-level backoff
- **Early validation**: rejects obviously invalid orders before pipeline entry (O(1) checks)
- **Input normalization**: ensures consistent order format

### 3. Order Management System (`src/core/oms.c`)
- Order validation (symbol, quantity, price, type)
- Local deterministic sequence number assignment
- Order lifecycle: NEW → ACK → PARTIAL → FILLED / CANCELED / REJECTED
- Hash-table order index for O(1) lookup

### 4. Pre-Trade Risk Engine (`src/core/risk_engine.c`)
- **Position limits** per user/symbol
- **Total exposure** monitoring (notional value)
- **Circuit breaker**: rejects orders when rate exceeds threshold
- **Kill switch**: atomic flag for emergency shutdown
- Multi-worker pool (configurable thread count)
- **V4**: Routes to Sequencer instead of directly to Matching

### 5. Sequencer (`src/core/sequencer.c`)
- Sits between Risk Engine and Matching Engine
- Assigns **global deterministic sequence IDs** to every order
- Enables snapshot + journal replay with total ordering
- Symbol-hash routing to matching engine shards
- MPSC input from Risk, MPSC fan-out to Matching shards

### 6. Matching Engine (`src/core/matching_engine.cpp` + `src/core/order_book.cpp`)
- Per-symbol order books with **skip-list** price levels (O(log N))
- **Price-time priority** matching with FIFO queues per price level
- Order types: **LIMIT**, **MARKET**, **IOC** (Immediate-or-Cancel), **FOK** (Fill-or-Kill)
- Bid/Ask sides with descending/ascending sort
- Sharded by symbol hash — each shard owns its order books exclusively
- **V4**: Receives globally-sequenced orders from Sequencer

### 7. Market Data Engine (`src/md/market_data.c`)
- Trade tick generation from matched orders
- Top-of-book and full depth snapshot support
- SPSC tick queue from matching engine
- Extensible to multicast/Kafka distribution

### 8. Persistence Journal (`src/persistence/journal.c`)
- Append-only file (AOF-style) for crash recovery
- Configurable `fsync` batching (default 1ms)
- 16 MB write buffer with ring-buffer input
- Journal entry types: NEW_ORDER, TRADE, CANCEL, SNAPSHOT

### 9. Event Bus (`src/core/event_bus.c`) — V5 NEW
- **Event Sourcing backbone**: all state changes produce immutable events
- Event types: `ORDER_CREATED`, `ORDER_REJECTED`, `ORDER_MATCHED`, `TRADE_EXECUTED`, `ORDER_CANCELED`
- Multi-producer, multi-consumer: matching engines publish, downstream systems subscribe
- Enables deterministic replay, audit trails, and cross-system integration
- Synchronous dispatch model (lock-free) for lowest latency

### 10. Clearing & Settlement (`src/core/clearing.c`) — V5 NEW
- Processes `TRADE_EXECUTED` events from the Event Bus
- Maintains per-account balances across assets
- **Double-entry ledger** with debit/credit entries per trade
- Automatic fee calculation (0.1%)
- Settlement statistics: trades settled, total notional, ledger entries

### 11. Shared Memory IPC (`src/ipc/shmem.c`)
- POSIX shared memory segments for cross-process communication
- Lock-free ring buffers in shared memory
- Supports multi-process deployment model

### 12. Memory Management

| Allocator | File | Use Case |
|-----------|------|----------|
| **Memory Pool** | `src/utils/memory_pool.c` | 4 GB slab, thread-local arenas, object recycling |
| **Slab Allocator** | `src/include/bt_slab_allocator.h`, `src/utils/slab_allocator.c` | Fixed-size Order/Trade/Event objects |
| **Lock-Free Pool** | `src/include/bt_lockfree_pool.h` | CAS-based freelist for MPSC object pooling |
| **HugePage Allocator** | `src/include/bt_hugepage.h`, `src/utils/hugepage.c` | 2MB/1GB pages for order books, MD buffers |
| **NUMA Allocator** | `src/include/bt_numa.h`, `src/utils/numa.c` | Per-NUMA-node memory allocation |

### 13. Concurrency Primitives

| Component | File | Description |
|-----------|------|-------------|
| **Lock-Free Queues** | `src/include/bt_lockfree_queue.h` | SPSC & MPSC ring buffers (C macros + C++ templates) |
| **Disruptor** | `src/include/bt_disruptor.h` | Multi-producer ring buffer with sequence claiming |
| **Queue Definitions** | `src/include/bt_queues.h` | Concrete queue types for the V4 pipeline |

### 14. Utilities
- **CPU Affinity** (`src/utils/cpu_affinity.c`): Thread pinning, `SCHED_FIFO` scheduling, `mlockall`, HugePages
- **Timer** (`src/utils/timer.c`): `clock_gettime` ns timer, `rdtsc`, latency histogram (p50/p95/p99/p99.9)
- **Logger** (`src/utils/logger.c`): Non-blocking ring-buffer logger, hot-path safe

## Data Flow

```
1. Client sends order via TCP (binary protocol)
2. Gateway authenticates, rate-limits, parses
3. Order Gate applies backpressure, early validation, traffic shaping
4. OMS validates, assigns local sequence number
5. Risk Engine checks position/exposure/circuit-breaker
6. Sequencer assigns global deterministic sequence ID
   → Routes to matching shard via hash(symbol) % N
7. Matching Engine executes against order book
   → Publishes events to Event Bus (ORDER_CREATED, TRADE_EXECUTED, etc.)
   → Trade ticks sent to Market Data engine
   → Order + Trade events journaled to persistence
8. Event Bus fans out events to subscribers:
   → Clearing & Settlement processes TRADE_EXECUTED
   → Journal writer persists all events
   → Market Data receives tick updates
9. Clearing settles trades: updates account balances, generates ledger entries
10. Order book snapshots published periodically
```

## Project Structure

```
aspira-broker/
├── bench/
│   └── benchmark.c            # Synthetic order load generator & latency analyzer
├── scripts/
│   ├── build.sh               # Build script (release/debug/clean)
│   ├── run.sh                 # Run script (server/bench modes)
│   └── bench.sh               # Benchmark suite (quick/default/full/custom)
├── src/
│   ├── include/               # Public headers (19 files)
│   │   ├── bt_types.h         # Core types: Order(64B), Trade, PriceLevel, Snapshots
│   │   ├── bt_config.h        # Compile/runtime configuration constants
│   │   ├── bt_lockfree_queue.h# SPSC & MPSC lock-free ring buffers (C/C++ compat)
│   │   ├── bt_queues.h        # V5 pipeline queue type definitions
│   │   ├── bt_memory_pool.h   # Pre-allocated slab memory pool API
│   │   ├── bt_slab_allocator.h# Fixed-size block allocator (C API + C++ template)
│   │   ├── bt_lockfree_pool.h # CAS-based lock-free object pool
│   │   ├── bt_hugepage.h      # HugePage allocator (2MB/1GB pages)
│   │   ├── bt_numa.h          # NUMA-aware memory allocation API
│   │   ├── bt_disruptor.h     # Disruptor pattern ring buffer
│   │   ├── bt_sequencer.h     # Sequencer API (global sequence ID assignment)
│   │   ├── bt_event.h         # Event types + Event Bus API (V5)
│   │   ├── bt_clearing.h      # Clearing & Settlement API (V5)
│   │   ├── bt_order_gate.h    # Order Gate API (V5)
│   │   ├── bt_order_book.h    # Order book: C API + C++ class wrapper
│   │   ├── bt_timer.h         # High-precision timer & latency stats
│   │   ├── bt_cpu.h           # CPU affinity, SCHED_FIFO, HugePages
│   │   ├── bt_journal.h       # Append-only journal API
│   │   └── bt_logger.h        # Ring-buffer logger API
│   ├── core/                  # Core trading logic (8 files)
│   │   ├── matching_engine.cpp# Matching engine (C++20, lock-free per shard)
│   │   ├── order_book.cpp     # Skip-list order book (C++20)
│   │   ├── sequencer.c        # Global sequencer (C11)
│   │   ├── order_gate.c       # Order Gate — traffic protection (C11, V5)
│   │   ├── event_bus.c        # Event Bus — event sourcing (C11, V5)
│   │   ├── clearing.c         # Clearing & Settlement (C11, V5)
│   │   ├── risk_engine.c      # Pre-trade risk checks (C11)
│   │   └── oms.c              # Order management system (C11)
│   ├── net/
│   │   └── gateway.c          # TCP gateway (C11, epoll)
│   ├── md/
│   │   └── market_data.c      # Market data publisher (C11)
│   ├── persistence/
│   │   └── journal.c          # AOF journal writer (C11)
│   ├── ipc/
│   │   └── shmem.c            # Shared memory IPC (C11)
│   ├── utils/                 # Utilities (7 files)
│   │   ├── memory_pool.c      # Pre-allocated slab + thread-local arenas
│   │   ├── slab_allocator.c   # Fixed-size block allocator
│   │   ├── hugepage.c         # HugePage-backed allocator
│   │   ├── numa.c             # NUMA-aware memory allocation
│   │   ├── timer.c            # High-precision timer & latency stats
│   │   ├── cpu_affinity.c     # CPU pinning & real-time scheduling
│   │   └── logger.c           # Ring-buffer logger
│   ├── main.c                 # Bootstrap, V4 pipeline orchestration, shutdown
│   └── CMakeLists.txt         # Build system (C11 + C++20)
├── docs/                      # Design documents
│   ├── broker_grade_trading_system_v2.md
│   └── high_performance_trading_system_v4.md
├── README.md
├── .gitignore
└── LICENSE
```

**~5,500 lines** of C and C++ across **40 files** (19 headers + 21 source files).

## Build & Run

### Prerequisites
- Linux (kernel 5.x+)
- GCC 13+ (or Clang 17+)
- CMake 3.16+
- libnuma (optional, for NUMA awareness)

### Build

```bash
# Quick build via script (recommended)
./scripts/build.sh release     # Release build
./scripts/build.sh debug       # Debug build with AddressSanitizer
./scripts/build.sh clean       # Clean build directory

# Manual build
mkdir build && cd build
cmake ../src -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Run

```bash
# Interactive TCP server mode
./scripts/run.sh server --port 9000

# Benchmark mode
./scripts/run.sh bench --bench 100000 --symbols 10 --matching-threads 4

# Benchmark suite
./scripts/bench.sh quick       # Smoke test (10k orders)
./scripts/bench.sh default     # Standard suite (50k + 100k)
./scripts/bench.sh full        # Stress test (50k → 500k)
```

### Send Orders via TCP

```bash
# LIMIT BUY order for SYM0001
echo -ne '\x00\x00\x00\x1aO u=100|s=SYM0001|p=100.50|q=100|d=B|t=L' | nc localhost 9000

# MARKET SELL order for SYM0001
echo -ne '\x00\x00\x00\x1aO u=100|s=SYM0001|p=0|q=50|d=S|t=M' | nc localhost 9000
```

**Binary Protocol Format:**
```
[4 bytes: total message length, network byte order]
[1 byte:  message type]
   'O' = New Order
   'C' = Cancel Order
[N bytes: payload]
   key=value|key=value|...
   Fields: u (user_id), s (symbol), p (price), q (quantity),
           d (side: B/S), t (type: L/M/I/F)
```

## Configuration

Key constants in `src/include/bt_config.h`:

| Constant | Default | Description |
|----------|---------|-------------|
| `BT_CFG_MATCHING_THREADS` | 4 | Matching engine shard count |
| `BT_CFG_RISK_THREADS` | 2 | Risk engine worker threads |
| `BT_CFG_MEMPOOL_SIZE_MB` | 4096 | Pre-allocated memory pool (NUMA-aware) |
| `BT_CFG_GATEWAY_PORT` | 9000 | TCP listen port |
| `BT_CFG_MAX_CONNECTIONS` | 1024 | Max concurrent connections |
| `BT_CFG_RATE_LIMIT_RPS` | 10000 | Max orders/sec per connection |
| `BT_CFG_RISK_MAX_POSITION` | 10,000,000 | Max position per symbol |
| `BT_CFG_RISK_MAX_EXPOSURE` | 50,000,000 | Max total notional exposure |
| `BT_CFG_RISK_CIRCUIT_BREAKER_THRESH` | 100,000 | Orders/sec trigger |
| `BT_CFG_JOURNAL_SYNC_MS` | 1 | Journal fsync interval |

Runtime overrides: `--port`, `--matching-threads`, `--bench`, `--symbols`, `--no-bench`.

## Hot-Path Design

The core order processing pipeline is **lock-free end-to-end**:

### Memory
- **Pre-allocated 4 GB slab**: no `malloc`/`free` on the hot path
- **Thread-local arenas**: each worker thread has a private bump allocator
- **Slab Allocator**: fixed-size block allocation with O(1) free-list recycling
- **Lock-Free Pool**: CAS-based freelist for multi-threaded object pooling
- **Object recycling**: freed order nodes return to per-thread free lists (O(1))
- **HugePages**: 2 MB pages reduce TLB misses; optional 1 GB pages
- **NUMA-aware**: `bt_numa_alloc_local()` allocates from local NUMA node
- **Cache-line alignment**: all queue slots and structures are 64-byte aligned
- **`mlockall`**: prevents paging of critical memory

### Queues
- **SPSC** (Single-Producer Single-Consumer): matching → market data, journal input
- **MPSC** (Multi-Producer Single-Consumer): each pipeline stage boundary
- **Disruptor**: C++ template for multi-producer ring buffer with sequence claiming
- All queues are power-of-2 sized, with head/tail on separate cache lines

### CPU
- **Core pinning**: critical threads on dedicated cores, I/O on isolated cores
- **SCHED_FIFO**: Sequencer 85, Matching 90, Risk 80, OMS 70, Gateway 60
- **`__builtin_ia32_pause`**: spin-wait hint in busy loops

## Threading Model

```
Core  0-1:  [OS / interrupts]      ← kept free for system
Core  2:    [Gateway] + [Order Gate]  SCHED_FIFO 55-60 (V5: gate co-located)
Core  3:    [OMS]                      SCHED_FIFO 70
Core  4:    [Risk Worker 0]  ─┐        SCHED_FIFO 80
Core  5:    [Risk Worker 1]  ─┤        work-stealing from shared MPSC
                              │
Core 11:    [Sequencer]       ─┤        SCHED_FIFO 85
              global seq ID    │        deterministic total ordering
              hash→shard       │
                              │
Core  6:    [Matching Engine 0]│        SCHED_FIFO 90 (shard 0)
Core  7:    [Matching Engine 1]│        SCHED_FIFO 90 (shard 1)
Core  8:    [Matching Engine 2]├──────  SCHED_FIFO 90 (shard 2)
Core  9:    [Matching Engine 3]│        SCHED_FIFO 90 (shard 3)
                              │
Core 10:    [Market Data]      │        SCHED_FIFO 50
Core 13:    [Clearing]         │        SCHED_FIFO 30 (V5 NEW, event-driven)
Core 12+:   [Journal Writer]           SCHED_FIFO 40 (async I/O)
```

- **Symbol-to-shard mapping**: `hash(symbol) % num_matchers` — deterministic by the Sequencer
- Each matching engine thread **owns** its order books exclusively → no locks needed
- Risk workers pull from a **shared MPSC queue** — work-stealing for load balancing
- Sequencer is **single-threaded** — total ordering requires a single sequence point

## Matching Engine

### Order Book Design
- **Skip list** for price levels: O(log N) insertion, lookup, and deletion
- **Bids**: sorted descending (highest price = best)
- **Asks**: sorted ascending (lowest price = best)
- Each price level maintains a **FIFO queue** of orders (price-time priority)
- Orders are stored as 64-byte cache-aligned structs

### Matching Algorithm

| Order Type | Behavior |
|------------|----------|
| **LIMIT** | Match against opposite side at or better than limit price. Unfilled remainder rests in the book. |
| **MARKET** | Match against best available liquidity. No resting — unfilled portion rejected. |
| **IOC** | Match what's available immediately. Cancel any remainder. |
| **FOK** | Fill entirely or cancel the whole order (no partial fills). |

### Cancel
- Remove order from price-level FIFO and order index
- If price level becomes empty, remove from skip list
- O(log N) for skip list operations, O(1) for index lookup

## Sequencer

The Sequencer is the **key V4 addition**, sitting between Risk and Matching:

```
Risk Engine → [bt_seq_in_queue_t] → Sequencer → [bt_match_in_queue_t × N] → Matching Shards
```

**Responsibilities:**
1. Receive risk-checked orders from the Risk Engine (MPSC queue)
2. Assign a **globally unique, monotonically increasing sequence ID**
3. Route to the correct matching shard via `hash(symbol) % N`
4. Output `bt_seq_match_msg_t{request, risk, local_seq, global_seq}`

**Why it matters:**
- **Deterministic replay**: same input order stream → same global seq IDs → same matching output
- **Snapshot + journal recovery**: replay from any snapshot with total ordering
- **Audit trail**: every order has a unique position in the global sequence
- **Cross-shard ordering**: enables future cross-symbol dependency resolution

## Risk Control

Implemented in the risk engine as **pre-trade checks**:

1. **Kill Switch**: Atomic flag — when set, all new orders are rejected immediately
2. **Circuit Breaker**: Rejects orders when the global rate exceeds a configurable threshold
3. **Position Limits**: Per-user, per-symbol maximum long/short position
4. **Exposure Check**: Total notional value across all positions
5. **Order Validation**: Zero quantity, invalid price, empty symbol

Risk checks are **parallelized** across multiple worker threads pulling from a shared MPSC queue. After passing, positions are updated and orders are forwarded to the Sequencer.

## Memory Management

The V4 system provides a **layered memory architecture**:

| Layer | Component | Allocation Pattern | Thread Safety |
|-------|-----------|-------------------|---------------|
| **Large objects** | HugePage Allocator | Bump allocator, 2MB-aligned | Single-owner |
| **Fixed-size hot** | Slab Allocator | Free-list, O(1) alloc/free | Single-owner (per thread) |
| **Multi-threaded** | Lock-Free Pool | CAS freelist | MPSC safe |
| **General purpose** | Memory Pool | Thread-local arenas | Per-thread |

No `malloc` or `free` is ever called on the critical order-processing path. All allocations come from pre-allocated, pre-faulted memory regions.

## Persistence & Recovery

- **Append-Only Journal**: all new orders, trades, and cancels are written sequentially
- **Batched fsync**: configurable interval (default 1ms) for durability/latency trade-off
- **Ring buffer**: decouples hot-path journal appends from disk I/O
- **Recovery model**: replay journal from last snapshot on restart
- **V4**: global sequence IDs enable deterministic replay

## Event Sourcing & Event Bus (V5)

The Event Bus is the **backbone of the V5 architecture**, implementing full event sourcing:

### Event Types
| Event | Trigger | Consumers |
|-------|---------|-----------|
| `ORDER_CREATED` | New resting limit order in book | Journal, Audit |
| `ORDER_REJECTED` | Order rejected by validation or risk | Journal, Gateway (response) |
| `ORDER_MATCHED` | Partial or full fill | Market Data, Journal |
| `TRADE_EXECUTED` | Completed trade (two orders matched) | **Clearing**, Journal, Market Data |
| `ORDER_CANCELED` | Order cancelled by user or system | Journal, Market Data |

### Architecture
- Matching engines **publish** events synchronously to the Event Bus
- Downstream systems **subscribe** with type-bitmask filters
- Events are **immutable** and **cache-line aligned**
- Enables deterministic replay: replay the event journal → reconstruct full system state

## Clearing & Settlement (V5)

The Clearing engine subscribes to `TRADE_EXECUTED` events:

### Per-Trade Processing
1. Identify buyer and seller accounts
2. Debit buyer: `balance -= notional + fee`
3. Credit seller: `balance += notional - fee`
4. Generate double-entry ledger entries (one per side)
5. Update cumulative trade statistics

### Features
- **0.1% trading fee** (configurable)
- **Double-entry ledger**: every trade produces matching debit/credit entries
- **Account model**: per-user, per-asset balances
- **Settled in V5 benchmark**: 37K trades, $6.3B notional, 73K ledger entries
- Future: netting engine, multi-currency support, external clearing house integration

## Market Data

- **Trade ticks**: generated for every match (symbol, price, quantity, side, timestamp)
- **Order book snapshots**: top-N price levels (configurable, default 10)
- **Snapshot interval**: configurable (default 100ms)
- Future: multicast (UDP) or Kafka distribution for production use

## Benchmarking

The built-in benchmark harness (`bench/benchmark.c`) generates synthetic order flow:

- Configurable order count and symbol universe
- Realistic order mix: 60% LIMIT, 20% MARKET, 10% IOC, 10% FOK
- Random price walk around base prices
- Alternating BUY/SELL sides
- **Latency percentiles**: p50, p95, p99, p99.9 with histogram
- **Throughput**: orders injected per second
- **Sequencer stats**: global sequence IDs assigned, drops

```bash
# Quick test
./scripts/bench.sh quick

# Standard suite
./scripts/bench.sh default

# Full stress test
./scripts/bench.sh full

# Custom
./scripts/run.sh bench --bench 500000 --symbols 50 --matching-threads 8
```

## V6 Isolation Architecture

V6 enforces **five layers of isolation** for production-grade fault containment:

### Process Isolation (Hard Boundary)
Each matching engine shard can run as an **independent OS process** via `fork()` + shared memory IPC:
```
Controller ──fork()──▶ [Shard 0 Process] Core 6  ──shmem── Input Queue
            ──fork()──▶ [Shard 1 Process] Core 7  ──shmem── Input Queue
            ──fork()──▶ [Shard 2 Process] Core 8  ──shmem── Input Queue
            ──fork()──▶ [Shard 3 Process] Core 9  ──shmem── Input Queue
```
- Crash containment: one shard failure ≠ system failure
- Independent restart: replay journal → restore state → resume
- Memory protection: kernel-enforced process boundaries

### Memory Tiering (Latency-Classified)
| Tier | Latency | Allocator | Use |
|------|---------|-----------|-----|
| **HOT** | < 100ns | Slab allocator, cache-aligned | Order nodes, trade records, hot queue slots |
| **WARM** | < 500ns | NUMA-local mmap, lock-free pools | IPC buffers, event bus ring, shared state |
| **COLD** | > 1μs | Heap (calloc), file-backed | Config, stats, persistence buffers, audit logs |

**Rule:** HOT never calls COLD. WARM bridges HOT ↔ COLD via Event Bus.

### Data Flow Isolation
- All inter-component flows are **unidirectional** lock-free queues
- Events are **immutable** once published to the Event Bus
- **No shared mutable state** between subsystems
- Single-writer rule: each data structure has exactly one owner thread

### Failure Domain Isolation
- Shard failure → contained to one symbol group
- Risk engine failure → new orders queued, matching continues
- Market data failure → trading uninterrupted
- Full recovery via **journal replay** (`bt_matching_replay_from_journal`)

### Branchless Hot Path
- Single conditional: `is_aggressive = (type != LIMIT)` drives the entire match path
- 4-way unrolled tick generation for vectorization
- `__builtin_ia32_pause()` in all spin loops

## Design Goals

| Goal | Status | Implementation |
|------|--------|---------------|
| Ultra-low latency | ✅ | Lock-free queues, CPU pinning, SCHED_FIFO, no malloc in hot path |
| High throughput | ✅ | **5.82M orders/sec**, 9-stage pipeline, sharded matching |
| **Deterministic execution** | ✅ | Global Sequencer with monotonic sequence IDs |
| **Event sourcing** | ✅ | **Event Bus with immutable typed events** |
| **Clearing & settlement** | ✅ | **Double-entry ledger, per-trade settlement, 73K+ entries** |
| Horizontal scalability | ✅ | Per-shard order books, configurable thread count |
| NUMA-aware memory | ✅ | Per-NUMA-node allocation, local-node binding |
| Fault tolerance | ✅ | AOF journal, kill switch, circuit breaker |
| **Traffic protection** | ✅ | **Order Gate with backpressure and request shaping** |
| Pre-allocated memory | ✅ | 4 GB slab + SlabAllocator + LockFreePool + HugePages |
| Cache optimization | ✅ | 64-byte alignment, false sharing prevention |
| Multi-process support | ✅ | Shared memory IPC module |
| Event pipeline | ✅ | Disruptor pattern + Event Bus fan-out |
| **Process isolation** | ✅ | **Fork-per-shard, shared-memory IPC, crash containment** |
| **Memory tiering** | ✅ | **HOT/WARM/COLD 3-tier with strict access rules** |
| **Branchless hot path** | ✅ | **Single-branch matching, 4-way unrolled tick emission** |
| **Hot standby** | ✅ | **Journal replay infrastructure for fault recovery** |
| Zero-GC hot path | ✅ | No malloc/free in any critical path |

## License

See [LICENSE](LICENSE) for details.

---

Built with **C11** and **C++20**. Architecture based on [`trading_system_architecture_v6.md`](docs/trading_system_architecture_v6.md).
