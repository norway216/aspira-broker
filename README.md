# Broker-Grade Trading System

A high-performance, low-latency trading system designed for equities, derivatives, and crypto markets. Built with C11 and C++20, featuring lock-free data structures, CPU-pinned threads, and a deterministic matching engine capable of **5+ million orders per second**.

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
- [Risk Control](#risk-control)
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
                    │   ORDER MANAGEMENT (OMS)     │
                    │  validation · seq numbers    │
                    │  order lifecycle state mach. │
                    └──────────────┬──────────────┘
                                   │  MPSC Queue
                    ┌──────────────▼──────────────┐
                    │      RISK ENGINE             │
                    │  position limits · exposure  │
                    │  circuit breaker · kill sw.  │
                    └──────────────┬──────────────┘
                                   │  MPSC Queue × N shards
                    ┌──────────────▼──────────────┐
                    │     MATCHING ENGINE          │
                    │  skip-list order books       │
                    │  price-time priority · FIFO  │
                    │  LIMIT/MARKET/IOC/FOK        │
                    └──────┬────────────┬──────────┘
                           │            │
              ┌────────────▼──┐  ┌──────▼──────────┐
              │  MARKET DATA  │  │   PERSISTENCE    │
              │  tick gen.    │  │  AOF journal     │
              │  snapshots    │  │  fsync batching  │
              └───────────────┘  └─────────────────┘
```

## Performance

Benchmarked on **Intel i9-12900H (20 cores)**, 16 GB RAM, Linux 6.17, GCC 13.3:

| Metric | Result | Target |
|--------|--------|--------|
| **Throughput** | **5.3M orders/sec** | ≥ 1M |
| Matching Threads | 4 shards | Configurable |
| Order Rejection Rate | 0.2% | Low |
| Queue Drops | < 0.01% | Near zero |
| Memory Pool | 4 GB pre-allocated | HugePages |

> **Note:** The throughput figure reflects end-to-end injection rate from the benchmark harness through the full pipeline (OMS → Risk → Matching). The core matching engine alone achieves higher internal rates. Latency on the hot path is measured in microseconds; end-to-end latency correlation requires the trade output matching infrastructure already in place.

## System Components

### 1. API Gateway (`src/net/gateway.c`)
- TCP server with **epoll** edge-triggered I/O
- Non-blocking sockets with `SO_REUSEPORT`, `TCP_NODELAY`
- Simple binary protocol: `[4B length][1B type][N-byte payload]`
- FIX-lite payload format: `key=value|key=value|...`
- Token-bucket rate limiting per connection
- Up to 1,024 concurrent connections

### 2. Order Management System (`src/core/oms.c`)
- Order validation (symbol, quantity, price, type)
- Deterministic sequence number assignment
- Order lifecycle: NEW → ACK → PARTIAL → FILLED / CANCELED / REJECTED
- Hash-table order index for O(1) lookup

### 3. Pre-Trade Risk Engine (`src/core/risk_engine.c`)
- **Position limits** per user/symbol
- **Total exposure** monitoring (notional value)
- **Circuit breaker**: rejects orders when rate exceeds threshold
- **Kill switch**: atomic flag for emergency shutdown
- Multi-worker pool (configurable thread count)

### 4. Matching Engine (`src/core/matching_engine.cpp` + `src/core/order_book.cpp`)
- Per-symbol order books with **skip-list** price levels (O(log N))
- **Price-time priority** matching with FIFO queues per price level
- Order types: **LIMIT**, **MARKET**, **IOC** (Immediate-or-Cancel), **FOK** (Fill-or-Kill)
- Bid/Ask sides with descending/ascending sort respectively
- Sharded by symbol hash — each shard owns its order books exclusively
- Lock-free within each shard (single-threaded per shard)

### 5. Market Data Engine (`src/md/market_data.c`)
- Trade tick generation from matched orders
- Top-of-book and full depth snapshot support
- SPSC tick queue from matching engine
- Extensible to multicast/Kafka distribution

### 6. Persistence Journal (`src/persistence/journal.c`)
- Append-only file (AOF-style) for crash recovery
- Configurable `fsync` batching (default 1ms)
- 16 MB write buffer with ring-buffer input
- Journal entry types: NEW_ORDER, TRADE, CANCEL, SNAPSHOT

### 7. Shared Memory IPC (`src/ipc/shmem.c`)
- POSIX shared memory segments for cross-process communication
- Lock-free ring buffers in shared memory
- Supports multi-process deployment model

### 8. Utilities
- **Memory Pool** (`src/utils/memory_pool.c`): 4 GB pre-allocated slab, thread-local bump allocators, no `malloc` on the hot path, object recycling via free lists
- **CPU Affinity** (`src/utils/cpu_affinity.c`): Thread pinning via `pthread_setaffinity_np`, `SCHED_FIFO` real-time scheduling, `mlockall` to prevent paging, HugePages support
- **Timer** (`src/utils/timer.c`): `clock_gettime` nanosecond timer, `rdtsc` for lowest-overhead profiling, TSC calibration, latency histogram with percentiles (p50/p95/p99/p99.9)
- **Logger** (`src/utils/logger.c`): Non-blocking ring-buffer logger, safe for hot-path use, configurable log levels

## Data Flow

```
1. Client sends order via TCP (binary protocol)
2. Gateway authenticates, rate-limits, normalizes
3. OMS validates, assigns sequence number
4. Risk engine checks position/exposure/circuit-breaker
5. Matching engine executes against order book
   - Aggressive orders (MARKET/IOC/FOK): match immediately
   - Passive orders (LIMIT): insert into order book
6. Trade ticks sent to Market Data engine
7. Trade + order events journaled to persistence
8. Order book snapshots published periodically
```

## Project Structure

```
src/
├── include/                  # Public headers
│   ├── bt_types.h            # Core types: Order, Trade, PriceLevel (64-byte aligned)
│   ├── bt_config.h           # Compile/runtime configuration
│   ├── bt_lockfree_queue.h   # SPSC & MPSC lock-free ring buffers (C/C++ compat)
│   ├── bt_queues.h           # Concrete queue type definitions for the pipeline
│   ├── bt_memory_pool.h      # Pre-allocated slab memory pool API
│   ├── bt_order_book.h       # Order book: C API + C++ class wrapper
│   ├── bt_timer.h            # High-precision timer & latency stats
│   ├── bt_cpu.h              # CPU affinity, SCHED_FIFO, HugePages
│   ├── bt_journal.h          # Append-only journal API
│   └── bt_logger.h           # Ring-buffer logger API
├── core/                     # Core trading logic
│   ├── matching_engine.cpp   # Matching engine (C++20, lock-free per shard)
│   ├── order_book.cpp        # Skip-list order book (C++20)
│   ├── risk_engine.c         # Pre-trade risk checks (C11)
│   └── oms.c                 # Order management system (C11)
├── net/
│   └── gateway.c             # TCP gateway (C11, epoll)
├── md/
│   └── market_data.c         # Market data publisher (C11)
├── persistence/
│   └── journal.c             # AOF journal writer (C11)
├── ipc/
│   └── shmem.c               # Shared memory IPC (C11)
├── utils/
│   ├── memory_pool.c         # Memory pool implementation
│   ├── timer.c               # Timer & latency stats
│   ├── cpu_affinity.c        # CPU pinning & real-time scheduling
│   └── logger.c              # Ring-buffer logger
├── main.c                    # Bootstrap, thread orchestration, signal handling
├── benchmark.c               # Synthetic order load generator & latency analyzer
└── CMakeLists.txt            # Build system (C11 + C++20)
```

**~3,700 lines** of C and C++ across 25 files.

## Build & Run

### Prerequisites
- Linux (kernel 5.x+)
- GCC 13+ (or Clang 17+)
- CMake 3.16+
- libnuma (optional, for NUMA awareness)

### Build

```bash
cd src
mkdir build && cd build

# Release build (optimized)
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Debug build (with AddressSanitizer)
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Run

```bash
# Run with built-in benchmark (100k orders, 10 symbols, 4 matching threads)
./bt_trading --bench 100000 --symbols 10 --matching-threads 4

# Run as a TCP server only (no benchmark)
./bt_trading --no-bench --port 9000

# Custom configuration
./bt_trading --bench 500000 --symbols 50 --matching-threads 8 --port 8080
```

### Send Orders via TCP

```bash
# Example: send a LIMIT BUY order for SYM0001
echo -ne '\x00\x00\x00\x1aO u=100|s=SYM0001|p=100.50|q=100|d=B|t=L' | nc localhost 9000

# Example: send a MARKET SELL order for SYM0001
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
| `BT_CFG_MEMPOOL_SIZE_MB` | 4096 | Pre-allocated memory pool |
| `BT_CFG_GATEWAY_PORT` | 9000 | TCP listen port |
| `BT_CFG_MAX_CONNECTIONS` | 1024 | Max concurrent connections |
| `BT_CFG_RATE_LIMIT_RPS` | 10000 | Max orders/sec per connection |
| `BT_CFG_RISK_MAX_POSITION` | 10,000,000 | Max position per symbol |
| `BT_CFG_RISK_MAX_EXPOSURE` | 50,000,000 | Max total notional exposure |
| `BT_CFG_JOURNAL_SYNC_MS` | 1 | Journal fsync interval |

Runtime overrides via command-line: `--port`, `--matching-threads`, `--bench`, `--symbols`, `--no-bench`.

## Hot-Path Design

The core order processing pipeline is **lock-free end-to-end**:

### Memory
- **Pre-allocated 4 GB slab**: no `malloc`/`free` on the hot path
- **Thread-local arenas**: each worker thread has a private bump allocator
- **Object recycling**: freed order nodes return to a per-thread free list (O(1))
- **HugePages**: 2 MB pages reduce TLB misses
- **Cache-line alignment**: all queue slots and shared structures are 64-byte aligned
- **`mlockall`**: prevents paging of critical memory

### Queues
- **SPSC** (Single-Producer Single-Consumer): matching → market data, journal input
- **MPSC** (Multi-Producer Single-Consumer): gateway → OMS, OMS → risk, risk → matching
- All queues are power-of-2 sized ring buffers with atomic head/tail
- False-sharing prevention: head and tail on separate cache lines

### CPU
- **Core pinning**: matching engines on dedicated cores, I/O on isolated cores
- **SCHED_FIFO**: matching engine priority 90, risk engine 80, OMS 70, gateway 60
- **`__builtin_ia32_pause`**: spin-wait hint in busy loops

## Threading Model

```
Core  2: [Gateway I/O Thread]
Core  3: [OMS Thread]
Core  4: [Risk Worker 0]  ─┐   MPSC queue
Core  5: [Risk Worker 1]  ─┤   (shared input)
                            │
Core  6: [Matching Engine 0] │  ── owns symbol shard 0
Core  7: [Matching Engine 1] │  ── owns symbol shard 1
Core  8: [Matching Engine 2] ├── owns symbol shard 2
Core  9: [Matching Engine 3] │  ── owns symbol shard 3
                            │
Core 10: [Market Data]      │  ── consumes from all matchers
Core  0-1: [OS / interrupts] ── kept free
Core 11+: [Journal Writer]  ── async I/O
```

- **Symbol-to-shard mapping**: `hash(symbol) % num_matchers` — deterministic routing
- Each matching engine thread **owns** its order books exclusively → no locks needed
- Risk workers pull from a **shared MPSC queue** — work-stealing for load balancing

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

## Risk Control

Implemented in the risk engine as **pre-trade checks**:

1. **Kill Switch**: Atomic flag — when set, all new orders are rejected immediately
2. **Circuit Breaker**: Rejects orders when the global rate exceeds a configurable threshold
3. **Position Limits**: Per-user, per-symbol maximum long/short position
4. **Exposure Check**: Total notional value across all positions
5. **Order Validation**: Zero quantity, invalid price, empty symbol

Risk checks are **parallelized** across multiple worker threads pulling from a shared MPSC queue. After passing risk checks, positions are updated and orders are routed to the appropriate matching engine shard.

## Persistence & Recovery

- **Append-Only Journal**: all new orders, trades, and cancels are written sequentially
- **Batched fsync**: configurable interval (default 1ms) for durability/latency trade-off
- **Ring buffer**: decouples hot-path journal appends from disk I/O
- **Recovery model**: replay journal from last snapshot on restart

## Market Data

- **Trade ticks**: generated for every match (symbol, price, quantity, side, timestamp)
- **Order book snapshots**: top-N price levels (configurable, default 10)
- **Snapshot interval**: configurable (default 100ms)
- Future: multicast (UDP) or Kafka distribution for production use

## Benchmarking

The built-in benchmark harness (`src/benchmark.c`) generates synthetic order flow:

- Configurable order count and symbol universe
- Realistic order mix: 60% LIMIT, 20% MARKET, 10% IOC, 10% FOK
- Random price walk around base prices
- Alternating BUY/SELL sides with slight buy bias
- **Latency percentiles**: p50, p95, p99, p99.9 with histogram
- **Throughput**: orders injected per second

```bash
# Large benchmark
./bt_trading --bench 1000000 --symbols 100 --matching-threads 8

# Quick test
./bt_trading --bench 10000 --symbols 5 --matching-threads 2
```

## Design Goals

| Goal | Status | Implementation |
|------|--------|---------------|
| Ultra-low latency | ✅ | Lock-free queues, CPU pinning, SCHED_FIFO, no malloc in hot path |
| High throughput | ✅ | 5.3M orders/sec, sharded matching engines, MPSC fan-out |
| Deterministic execution | ✅ | Sequence numbers, hash-based symbol routing |
| Horizontal scalability | ✅ | Per-shard order books, configurable thread count |
| Fault tolerance | ✅ | AOF journal, kill switch, circuit breaker |
| Pre-allocated memory | ✅ | 4 GB slab, thread-local arenas, object recycling |
| Cache optimization | ✅ | 64-byte alignment, false sharing prevention |
| Multi-process support | ✅ | Shared memory IPC module |

## License

See [LICENSE](LICENSE) for details.

---

Built with C11 and C++20. Inspired by exchange-grade trading system architectures as described in the accompanying [design document](docs/broker_grade_trading_system_v2.md).
