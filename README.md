# Aspira Broker — Exchange-Grade Trading System (V7)

A high-performance, pure-C electronic trading platform with a 9-stage lock-free pipeline, global sequencing, event sourcing, fixed-point precision, and zero heap allocation in the critical path. Built for determinism, fault isolation, and microsecond-level latency.

> **2026-06**: Seven optimization rounds applied — see [`docs/optimization_report.md`](docs/optimization_report.md) for the complete history (55 fixes across 29 files).

> Based on `docs/trading_system_architecture_v7.md`

---

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [Quick Start](#quick-start)
- [Pipeline](#pipeline)
- [System Components](#system-components)
- [Wire Protocol](#wire-protocol)
- [Configuration](#configuration)
- [Hot-Path Design](#hot-path-design)
- [Memory Management](#memory-management)
- [Threading Model](#threading-model)
- [Matching Engine](#matching-engine)
- [Risk Control](#risk-control)
- [Persistence & Recovery](#persistence--recovery)
- [Benchmarking](#benchmarking)
- [Design Goals](#design-goals)
- [Optimization History](#optimization-history)
- [License](#license)

---

## Architecture Overview

```
                         ┌──────────────────────┐
                         │      CLIENTS          │
                         │  TCP / Binary / Text  │
                         └──────────┬───────────┘
                                    │
                     ┌──────────────▼──────────────┐
                     │       API GATEWAY            │
                     │  epoll ET · auth · rate lim. │
                     │  ring-buffer send + recv     │
                     └──────────────┬──────────────┘
                                    │  MPSC
                     ┌──────────────▼──────────────┐
                     │        ORDER GATE            │
                     │  backpressure · validation   │
                     └──────────────┬──────────────┘
                                    │  MPSC
                     ┌──────────────▼──────────────┐
                     │           OMS               │
                     │  order/cancel relay          │
                     └──────────────┬──────────────┘
                                    │  MPSC
                     ┌──────────────▼──────────────┐
                     │     RISK ENGINE (×N)         │
                     │  per-user exposure · breaker │
                     └──────────────┬──────────────┘
                                    │  MPSC
                     ┌──────────────▼──────────────┐
                     │        SEQUENCER             │
                     │  global seq ID · symbol hash │
                     └──────────────┬──────────────┘
                                    │  MPSC × N
                     ┌──────────────▼──────────────┐
                     │   MATCHING ENGINE (×N)       │
                     │  branchless skip-list (C)    │
                     │  fixed-point price matching  │
                     └──────┬────────┬──────┬───────┘
                            │        │      │
               ┌────────────▼──┐ ┌───▼───┐ │
               │  MARKET DATA  │ │ JRNL  │ │
               └───────────────┘ └───────┘ │
                                    ┌──────▼──────────┐
                                    │   EVENT BUS      │
                                    │ pub/sub dispatch  │
                                    └──────┬──────────┘
                                           │
                              ┌────────────┼────────────┐
                              ▼            ▼            ▼
                         ┌─────────┐ ┌──────────┐ ┌─────────┐
                         │CLEARING │ │PERSIST   │ │RECOVERY │
                         │ledger   │ │journal   │ │replay   │
                         └─────────┘ └──────────┘ └─────────┘
```

---

## Quick Start

### Prerequisites
- Linux (kernel 5.x+)
- GCC 13+ (or Clang 17+)
- CMake 3.16+
- libnuma (optional)

### Build

```bash
# Manual build
mkdir build && cd build
cmake ../src -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

**Note**: The project is pure C (C11). No C++ compiler is required.

### Run

```bash
# Server mode (no benchmark)
./bt_trading --port 9000 --no-bench

# Benchmark mode
./bt_trading --bench 50000 --symbols 10

# Process isolation mode (fork-per-shard)
./bt_trading --port 9000 --no-bench --isolated
```

### CLI Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | 9000 | TCP listen port |
| `--bench N` | 100000 | Run benchmark with N orders |
| `--symbols N` | 10 | Number of benchmark symbols |
| `--matching-threads N` | 4 | Matching engine shards (power of 2, ≤8) |
| `--no-bench` | — | Disable benchmark, server-only |
| `--isolated` | — | Use process isolation for matching |

---

## Pipeline

```
Gateway → OrderGate → OMS → Risk×2 → Sequencer → Match×4 → MD → EventBus → Clearing
  MPSC      MPSC      MPSC    MPSC       MPSC        MPSC     SPSC     sync
```

- **9 stages**, all connected by lock-free MPSC/SPSC queues
- **Downstream-first startup**: consumers start before producers
- **Upstream-first shutdown**: producers stop before consumers
- **All cross-thread flags** use `__atomic_*` (RELAXED ordering)
- **Response path**: matching engine → gateway for client confirmations

---

## System Components

### 1. API Gateway (`src/net/gateway.c`)
- TCP server with **epoll** edge-triggered I/O
- **Auth**: message type `'A'` with API key whitelist
- **Order**: type `'O'` (text) and `'B'` (binary)
- **Cancel**: type `'C'` (binary cancel request)
- **Response**: type `'R'` (order confirmations back to clients)
- Ring-buffer recv (O(1) message consumption) + ring-buffer send (EPOLLOUT)
- Connection pool reuse + 60s idle timeout
- Token-bucket rate limiting per connection

### 2. Order Gate (`src/core/order_gate.c`)
- O(1) early validation (symbol, quantity, price, side, type)
- Backpressure: monitors downstream queue depth, throttles when full

### 3. Order Management System (`src/core/oms.c`)
- Relays orders and cancels from Order Gate to Risk Engine
- Copies `msg_type` field for downstream cancel routing

### 4. Pre-Trade Risk Engine (`src/core/risk_engine.c`)
- **Per-user notional exposure**: CAS-tracked, per-user limits (Round 4)
- **Per-symbol position limits**: CAS-based, per `(user_id, symbol)`
- **Circuit breaker**: sliding-window rate tracker, trips at 100K orders/sec
- **Kill switch**: atomic emergency stop
- **Deferred notional commit**: only after ALL checks pass (Round 5)
- Multi-worker pool pulling from shared MPSC queue

### 5. Sequencer (`src/core/sequencer.c`)
- Monotonically increasing global sequence IDs
- Symbol FNV-1a hash routing to matching shards
- Power-of-2 bitmask routing (not modulus)
- Exponential backoff on full output queue

### 6. Matching Engine (`src/core/order_book.c` + `src/core/matching_engine.c`)
- **Pure C**: skip-list with macro-generated branchless BID/ASK traversal (Round 7)
- **Fixed-point prices**: `int64_t` micro-dollar precision (Round 7)
- **Preallocated**: SlNode slab (8192 nodes) + order index hash table (65536 buckets)
- **Zero heap allocation** in insert/match/cancel hot path
- **O(1) FOK pre-check** via cached total quantity
- **O(1) cancel** via hash table index
- Order types: LIMIT, MARKET, IOC, FOK
- Price-time priority with FIFO queues per price level
- Per-stage latency measurement (avg/max) output at shutdown

### 7. Market Data Engine (`src/md/market_data.c`)
- Trade tick generation from matched orders
- SPSC tick queue from matching engine

### 8. Persistence Journal (`src/persistence/journal.c`)
- Async batch writes (no `O_DSYNC` — uses periodic `fdatasync`)
- Batch drain: up to 64 entries per loop iteration
- 16 MB write buffer with short-write recovery
- Entry types: NEW_ORDER, TRADE, CANCEL

### 9. Event Bus (`src/core/event_bus.c`)
- Subscriber registration with type-bitmask filters
- Handler snapshot under mutex, dispatch outside lock (concurrent across shards)
- Unsubscribe API for safe handler removal
- Event types: ORDER_CREATED, ORDER_REJECTED, ORDER_MATCHED, TRADE_EXECUTED, ORDER_CANCELED, CIRCUIT_BREAKER, KILL_SWITCH

### 10. Clearing & Settlement (`src/core/clearing.c`)
- Event-driven: subscribes to TRADE_EXECUTED
- Per-user account tracking via `buy_user_id`/`sell_user_id`
- Double-entry ledger entries
- 0.1% fee calculation

### 11. Crash Recovery (`src/core/recovery.c`)
- Reads append-only journal at startup
- Replays entries to restore sequence numbers and order/trade counts
- Graceful cold start if journal is missing or empty

### 12. Process Isolation (`src/core/shard_ipc.c`)
- `fork()` per matching shard with shared-memory IPC queues
- Child process creates private memory arena and journal
- Calls real matching engine loop (`matching_thread()`)
- Default mode is in-process pthreads; `--isolated` enables fork mode

### 13. Memory Infrastructure

| Allocator | File | Use Case |
|-----------|------|----------|
| **Hash Table** | `src/include/bt_hashmap.h` | Order index (uint64→void*, preallocated) |
| **Memory Pool** | `src/utils/memory_pool.c` | Thread-local arenas, order node recycling |
| **Slab Allocator** | `src/utils/slab_allocator.c` | Skip-list SlNode blocks (thread-safe CAS) |
| **Lock-Free Pool** | `src/include/bt_lockfree_pool.h` | CAS-based MPSC object pooling |
| **HugePage Allocator** | `src/utils/hugepage.c` | 2MB pages for large buffers |
| **NUMA Allocator** | `src/utils/numa.c` | Per-NUMA-node allocation |

### 14. Concurrency Primitives

| Component | File |
|-----------|------|
| **Lock-Free Queues** | `src/include/bt_lockfree_queue.h` (SPSC & MPSC) |
| **Disruptor** | `src/include/bt_disruptor.h` |
| **Queue Types** | `src/include/bt_queues.h` (pipeline message definitions) |

---

## Wire Protocol

### Type `'A'` — Authentication
```
[4B len][1B 'A'][api_key(N bytes)]
```
Required before any trading message. Built-in test keys: `"test-key-..."`, `"benchmark-key-..."`.

### Type `'O'` — Text Order (FIX-lite)
```
[4B len][1B 'O'][payload: key=value|key=value|...]
Fields: u (user_id), s (symbol), p (price), q (quantity), d (B/S), t (L/M/I/F)
```

### Type `'B'` — Binary Order
```
[4B len][1B 'B'][raw bt_order_request_t struct]
Server-side timestamp overwritten on arrival.
```

### Type `'C'` — Cancel Request
```
[4B len][1B 'C'][order_id(8B)][user_id(8B)][symbol(16B)]
```

### Type `'R'` — Response (server → client)
```
[4B len][1B 'R'][bt_order_response_t struct]
```

---

## Configuration

Key constants in `src/include/bt_config.h`:

| Constant | Default | Description |
|----------|---------|-------------|
| `BT_CFG_MATCHING_THREADS` | 4 | Matching shards (power of 2) |
| `BT_CFG_RISK_THREADS` | 2 | Risk worker threads |
| `BT_CFG_GATEWAY_PORT` | 9000 | TCP listen port |
| `BT_CFG_MAX_CONNECTIONS` | 1024 | Max concurrent connections |
| `BT_CFG_RATE_LIMIT_RPS` | 10000 | Max orders/sec per connection |
| `BT_CFG_RISK_MAX_POSITION` | 10,000,000 | Max position per symbol |
| `BT_CFG_RISK_MAX_EXPOSURE` | 50,000,000 | Max per-user notional |
| `BT_CFG_RISK_CIRCUIT_BREAKER_THRESH` | 100,000 | Orders/sec breaker trigger |
| `BT_CFG_JOURNAL_SYNC_MS` | 1 | Journal fsync interval |
| `BT_CFG_MEMPOOL_SIZE_MB` | 4096 | Memory pool size |

**Price precision**: `bt_price_t` = `int64_t`, scale = 1,000,000 (micro-dollar). IEEE 754 floating-point NOT used for price comparisons — all matching uses exact integer arithmetic.

---

## Hot-Path Design

### Zero Heap Allocation
The entire insert→match→cancel path uses **zero `malloc`/`free` calls**:

| Allocation | Mechanism | Preallocation |
|---|---|---|
| Order nodes | `bt_mempool_alloc_order` (arena) | 4 GB shared pool |
| Skip-list nodes | `bt_slab_alloc` (per-book slab) | 8192 nodes per book |
| Order index lookup | `bt_hashmap_get/put` (array) | 65536 buckets per book |
| Book cache lookup | Linear array scan | 1024 entries per shard |
| Trade output | Fixed stack array | `trades[256]` |

### Branchless Skip-List
BID/ASK traversal functions are macro-generated — each instantiation has a single-direction comparison with no ternary branch inside the loop.

### Memory Ordering
All cross-thread atomics use `__ATOMIC_RELAXED`. The `pthread_join` barrier at shutdown provides the final synchronization. Statistics counters are single-writer — relaxed accesses are safe.

### CPU
- Dedicated core per matching shard (no context switching)
- `SCHED_FIFO` real-time scheduling priority 90
- `BT_CPU_PAUSE()` portable spin-wait (x86 `pause`, ARM `yield`)

---

## Memory Management

| Tier | Latency | Allocator | Use |
|------|---------|-----------|-----|
| **HOT** | < 100ns | Slab allocator, hash table | Order nodes, skip-list nodes, index |
| **WARM** | < 500ns | Arena bump allocator | Order requests, events |
| **COLD** | > 1μs | Heap (init/shutdown only) | Config, stats, journal buffers |

**Per-book memory budget**: ~2.2 MB (1.2 MB SlNode slab + 1 MB hash table)
**Per-shard maximum**: ~2.3 GB (1024 symbols × 2.2 MB)

---

## Threading Model

```
Core  0-1:  [OS / interrupts]
Core  2:    [Gateway] + [Order Gate]     SCHED_FIFO 55-60
Core  3:    [OMS]                        SCHED_FIFO 70
Core  4-5:  [Risk Workers ×2]            SCHED_FIFO 80
Core  6-9:  [Matching Shards ×4]         SCHED_FIFO 90
Core 10:    [Market Data]                SCHED_FIFO 50
Core 11:    [Sequencer]                  SCHED_FIFO 85
Core 12+:   [Journal Writer]             SCHED_FIFO 40
Core 13:    [Clearing]                   SCHED_FIFO 30
```

- Symbol-to-shard routing: `FNV-1a_hash(symbol) & (num_shards - 1)` — deterministic
- Each matching shard owns its order books exclusively — no locks
- Risk workers: shared MPSC queue with work-stealing

---

## Matching Engine

### Order Book
- Skip-list price levels (O(log N) insert/lookup/delete)
- Bids: descending (highest price = best)
- Asks: ascending (lowest price = best)
- Each price level: FIFO queue of orders (price-time priority)
- 64-byte cache-aligned order structs

### Order Types

| Type | Behavior |
|------|----------|
| **LIMIT** | Match at or better than limit price. Unfilled remainder rests. |
| **MARKET** | Match at best available. No resting. |
| **IOC** | Immediate-or-Cancel. Discard unfilled. |
| **FOK** | Fill-or-Kill. Two-phase price-aware check. All-or-nothing. |

### Price Precision
All prices are `int64_t` fixed-point (micro-dollars). Exact integer comparison — no floating-point epsilon issues.

---

## Risk Control

1. **Kill Switch**: atomic flag — when set, all new orders rejected
2. **Circuit Breaker**: sliding-window rate tracker, trips at configurable threshold
3. **Per-User Exposure**: CAS-tracked notional per user
4. **Position Limits**: per `(user_id, symbol)`, CAS-based updates
5. **Order Validation**: zero quantity, negative price, excessive size

---

## Persistence & Recovery

- **Async batch journal**: periodic `fdatasync`, no `O_DSYNC`
- **Batch drain**: 64 entries per iteration, capacity-triggered sync
- **Recovery at startup**: replays journal to restore sequence numbers
- **Verified**: 2,314 entries replayed from previous run (Round 4)

---

## Benchmarking

```bash
./bt_trading --bench 50000 --symbols 10
```

Output includes:
- **Throughput**: orders injected per second
- **Injection interval**: p50/p95/p99/p99.9 (time between pushes)
- **Pipeline stats**: sequencer sequences, event bus delivered, clearing settled
- **Per-shard latency**: average and maximum at shutdown (Round 7)

---

## Design Goals

| Goal | Status | Implementation |
|------|--------|---------------|
| Pure C | ✅ | C11 only, zero C++ dependencies |
| Fixed-point price | ✅ | `int64_t` micro-dollar, no IEEE 754 comparisons |
| Zero hot-path heap | ✅ | All allocations preallocated |
| Lock-free pipeline | ✅ | MPSC/SPSC queues end-to-end |
| Deterministic execution | ✅ | Global Sequencer + monotonic IDs |
| Event sourcing | ✅ | Event Bus with immutable typed events |
| Clearing & settlement | ✅ | Double-entry ledger, per-user accounts |
| Crash recovery | ✅ | Journal replay at startup |
| Circuit breaker | ✅ | Sliding-window rate tracker |
| Process isolation | ✅ | Fork-per-shard with shared-memory IPC |
| Branchless matching | ✅ | Macro-generated BID/ASK skip-list traversal |
| Client responses | ✅ | EPOLLOUT send path + response queue |
| Cancel support | ✅ | End-to-end cancel through full pipeline |
| API auth | ✅ | API key whitelist at gateway |
| Per-stage latency | ✅ | Matching engine avg/max at shutdown |
| CPU affinity | ✅ | Dedicated cores, SCHED_FIFO |
| NUMA-aware memory | ✅ | Per-NUMA-node allocation |
| Cache optimization | ✅ | 64-byte alignment, false sharing prevention |

---

## Optimization History

Seven optimization rounds applied (June 2026). **55 fixes across 29 files.** Full details:

📄 **[`docs/optimization_report.md`](docs/optimization_report.md)**

| Round | Focus | Fixes |
|-------|-------|-------|
| 1 | Infrastructure | 14 — MPSC bug, -ffast-math, missing modules, gateway, memory |
| 2 | Concurrency + Hot-path | 11 — Data races, atomics, observability |
| 3 | Architecture | 10 — Journal, event bus, FOK, routing, risk, trade_t |
| 4 | Architecture Features | 7 — Response, breaker, recovery, cancel, exposure, auth, isolation |
| 5 | Correctness | 5 — Response scope, notional ordering, CPU bounds, NULL checks, FOK truncation |
| 6 | V7 C Conversion | 5 — Hash table, pure C order book, pure C matching, C-only build |
| 7 | Precision + Performance | 3 — Fixed-point prices, branchless skip-list, latency measurement |

---

## License

See [LICENSE](LICENSE) for details.

---

Built with **C11**. Architecture based on [`docs/trading_system_architecture_v7.md`](docs/trading_system_architecture_v7.md).
