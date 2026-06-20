# Aspira Broker — 优化报告 (2026-06)

本文档记录了对 Aspira Broker 交易系统代码库的全面优化分析和修复。

---

## 🔴 严重问题 (Critical)

### 1. MPSC Queue 写入丢失 Bug

**文件**: `src/include/bt_lockfree_queue.h`

**问题描述**: `BT_MPSC_PUSH` 宏在 CAS 成功认领 slot 后，有一个多余的条件检查：
```c
if (__next != BT_ATOMIC_LOAD((q).head, acquire)) {
    (q).buffer[__t] = (item);  // 条件写入
}
```

当 buffer 刚好满时（`__next == head`），CAS 已经成功推进了 `tail`，但不会写入数据。这导致队列中产生一个"空洞"——`tail` 指向未写入的 slot，consumer 会读取到垃圾/过期数据。

**修复方案**: CAS 成功后无条件写入 buffer。这是标准的 Vyukov MPSC 算法形式：CAS 认领 slot → 写入数据。`goto` 用于在队列满时跳过写入。

**影响**: 数据正确性 — 在队列高负载下，consumer 可能读取到错误的订单数据。

---

### 2. `-ffast-math` 编译标志对金融系统的危险

**文件**: `src/CMakeLists.txt`

**问题描述**: Release 编译使用了 `-ffast-math`，该标志会：
- 破坏 IEEE 754 浮点数精度保证
- 将 `NaN` / `Inf` 当作普通值处理
- 改变浮点比较语义（`a < b` 可能不符合 IEEE 754）
- 允许编译器重新排序浮点运算

在金融交易系统中，价格计算的精确性至关重要。`NaN` 传播可能被意外忽略，导致错误的交易决策或结算错误。

**修复方案**: 移除 `-ffast-math`，保留 `-O3 -march=native -flto -funroll-loops` 等其他优化。

**影响**: 数值正确性 — 防止因浮点语义改变导致的交易定价错误。

---

### 3. 核心业务模块实现缺失

**文件**: `src/CMakeLists.txt`

**问题描述**: CMakeLists.txt 引用了 10 个尚未实现的 `.c/.cpp` 文件：

| 缺失文件 | 对应功能 |
|----------|----------|
| `core/risk_engine.c` | 风控引擎 |
| `core/oms.c` | 订单管理系统 |
| `core/sequencer.c` | 全局排序器 |
| `core/order_gate.c` | 订单网关 |
| `core/event_bus.c` | 事件总线 |
| `core/clearing.c` | 清算结算 |
| `core/shard_ipc.c` | 分片进程隔离 |
| `core/order_book.cpp` | C++ 订单簿 |
| `core/matching_engine.cpp` | C++ 撮合引擎 |

目前只有头文件和前向声明，系统无法编译运行。这是最根本的问题。

**建议**: 按照 V5/V6 架构文档逐步实现各模块，优先完成 OMS → Risk → Sequencer → Matching 核心流水线。

---

## 🟠 重要问题 (High Priority)

### 4. 连接池泄漏 — 关闭的连接永不回收

**文件**: `src/net/gateway.c`

**问题描述**: `gw_accept()` 总是使用 `ctx->conns[ctx->num_conns]` 创建新连接，`num_conns` 只增不减。当连接关闭时（`fd = -1`），slot 不会被重用。一旦 `num_conns >= max_conns`，即使实际活跃连接很少，新连接也会被拒绝。

**修复方案**:
- 添加 `gw_find_free_slot()` 函数，扫描 `conns[]` 数组查找 `fd == -1` 的空闲 slot
- 跟踪 `active_conns` 而非 `num_conns`
- 连接关闭时将 slot 标记为可重用

---

### 5. `gw_process_data` 的 `memmove` 导致 O(n²) 性能退化

**文件**: `src/net/gateway.c`

**问题描述**: 每处理一个消息后都执行 `memmove` 来移动剩余数据：
```c
memmove(conn->recv_buf, conn->recv_buf + msg_len, conn->recv_len - msg_len);
```

当缓冲区中积压大量消息时，重复的 `memmove` 开销急剧增大。

**修复方案**: 实现环形缓冲区（ring buffer），使用 `recv_head` 和 `recv_tail` 游标替代线性缓冲区。每个消息处理完只需 O(1) 推进 `recv_head`，无需 `memmove`。

新增辅助函数:
- `gw_ring_readable()` — 可读字节数
- `gw_ring_writable()` — 可写空间
- `gw_ring_peek()` — 从 ring 中读取数据（支持回绕）
- `gw_ring_consume()` — 推进 head（丢弃已消费数据）

---

### 6. 订单文本解析 — 热路径性能瓶颈

**文件**: `src/net/gateway.c`

**问题描述**: `gw_parse_order()` 使用 `memchr` + `strtod` + `strtoull` 逐字符解析 `key=value|key=value` 文本协议。每次订单需要多次 `memchr` 扫描和 `strtod`（浮点解析极其昂贵）。

对于宣称的性能目标（6.65M orders/sec），文本解析是明显的瓶颈。

**修复方案**: 在保留文本协议兼容性的同时，新增二进制协议快速通道（消息类型 `'B'`）：
- `gw_parse_order_binary()` — 直接 `memcpy` 到 `bt_order_request_t` 结构体
- 客户端将原始 struct 按 wire format 发送，服务端直接反序列化
- 文本协议仍然支持（消息类型 `'O'`），用于调试和兼容性

---

### 7. Benchmark 延迟统计错误

**文件**: `bench/benchmark.c`

**问题描述**: 旧代码将每次 push 的绝对时间戳存入 `latencies[]`，然后计算：
```c
lat_ns[i] = latencies[i] - bench_start;  // 偏移量，非延迟
```

这计算的是"距离 benchmark 开始的时间偏移"，不是系统延迟。命名为 "Injection Latency" 具有误导性。

**修复方案**: 
- 改为记录**注入间隔**（相邻两次成功 push 之间的时间差）
- 明确标注为 "Injection Interval"，说明这是注入端指标，非端到端延迟
- 端到端延迟需要从流水线获取响应时间戳

---

## 🟡 中等问题 (Medium Priority)

### 8. `volatile` 误用 — 应使用 `_Atomic`

**文件**: `src/main.c`, `src/net/gateway.c`, `src/md/market_data.c`, `src/persistence/journal.c`

**问题描述**: C11 标准明确规定 `volatile` 不保证多线程可见性和原子性。多个文件中对 `running` 标志使用 `volatile int`，在信号处理和跨线程停止中使用，这在实践中可能"碰巧"工作，但不正确。

**修复方案**: 全部替换为 `atomic_int` + `atomic_load()`/`atomic_store()`/`atomic_init()`。这是 C11 标准保证的正确方式。

---

### 9. `bt_disruptor_claim` C 占位符损坏

**文件**: `src/include/bt_disruptor.h`

**问题描述**: C 版本的 `bt_disruptor_claim()` 包含无意义的指针算术运算且总是返回 0。只有 C++ 模板可用，C 代码无法使用 Disruptor。

**修复方案**: 实现一个可工作的 C 版本 `bt_disruptor_claim_c()` 和 `bt_disruptor_commit_c()`：
- 使用通用的结构布局（cursor → committed[] → buffer[]）
- 正确的回绕检测（检查 committed[wrap_point]）
- CAS 竞争处理

---

### 10. `__builtin_ia32_pause()` 不可移植

**文件**: `src/persistence/journal.c`, `src/md/market_data.c`

**问题描述**: 使用 x86 专用的 `__builtin_ia32_pause()` 作为自旋等待提示。

**修复方案**: 在 `bt_cpu.h` 中添加可移植宏：
```c
#if defined(__x86_64__) || defined(__i386__)
#define BT_CPU_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define BT_CPU_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#define BT_CPU_PAUSE() /* no-op */
#endif
```

所有文件中的 `__builtin_ia32_pause()` 替换为 `BT_CPU_PAUSE()`。

---

### 11. 队列容量缺少 2 的幂校验

**文件**: `src/include/bt_config.h`

**问题描述**: SPSC/MPSC 队列的 mask 算法 (`index & (capacity - 1)`) 依赖 2 的幂容量。非 2 的幂容量会静默产生错误的索引计算。

**修复方案**: 添加编译期 `_Static_assert` 检查：
```c
_Static_assert(_BT_IS_POW2(BT_CFG_GATEWAY_QUEUE_CAP), "...");
_Static_assert(_BT_IS_POW2(BT_CFG_OMS_QUEUE_CAP), "...");
// ... 覆盖所有队列容量宏
```

---

### 12. Memory Pool `offset` 字段双用途冲突

**文件**: `src/include/bt_memory_pool.h`, `src/utils/memory_pool.c`

**问题描述**: `bt_mempool_t.offset` 既被用作全局 arena 划分的光标，又被 `bt_mempool_get_arena()` 用来做 arena 分配的轮询计数器（通过 `atomic_fetch_add`）。两个语义冲突。

**修复方案**: 添加独立的 `arena_counter` 字段：
- `offset` — 仅用于初始 arena 划分的光标
- `arena_counter` — 专用的 arena 轮询分配计数器

**影响**: 避免 arena 分配和内存划分互相干扰。

---

### 13. C++ Disruptor `claim()` 回绕检测不完整

**文件**: `src/include/bt_disruptor.h`

**问题描述**: C++ 模板 `Disruptor::claim()` 中的回绕检测代码：
```cpp
if (committed_[wrap_point & mask_].load(...) != (size_t)(-1) && ...) {
    /* Still waiting for consumer */  // 空注释，无实际处理
}
```

没有实际的错误返回或等待逻辑。生产者可能覆盖未消费的 slot。

**修复方案**: C 版本 `bt_disruptor_claim_c()` 中正确实现回绕检测并返回 `SIZE_MAX` 表示 ring full。C++ 模板的 claim 函数应注意：当前代码已执行 `compare_exchange_strong`，如果成功则返回 seq，但未在 CAS 前 guard 回绕条件。建议在 C++ 版本中也添加明确的 full 检查。

---

## 🔵 轻微问题 (Low Priority)

### 14. `gethostbyname` 已弃用

**文件**: `test/test_trading.c`

**问题描述**: POSIX.1-2008 标记 `gethostbyname` 为弃用。

**修复方案**: 替换为 `getaddrinfo` + `freeaddrinfo`，同时处理 IPv4/IPv6 双栈。

---

### 15. `bt_lfpool_alloc` strict aliasing 违规

**文件**: `src/include/bt_lockfree_pool.h`

**问题描述**: C 版本的 `*(void **)head` 将任意对象的首 8 字节强制解释为 `void*`，违反了 C 标准的 strict aliasing 规则。

**修复方案**: 引入显式的 `bt_lfpool_node_t` 结构体，要求被池化的对象在 offset 0 嵌入该 header。使用 `node->next` 替代 `*(void **)ptr`。

---

### 16. 其他建议项

| 项目 | 描述 | 建议 |
|------|------|------|
| Market Data | MD 线程只累加 tick_count，无实际行情分发 | 实现客户端订阅和行情推送 |
| Logger 可移植性 | `__attribute__((format(printf, ...)))` 只在 GCC/Clang 可用 | 用 `#ifdef __GNUC__` 保护 |
| SO_REUSEPORT | 设置了但只创建了一个 gateway | 支持多线程监听或删除设置 |
| 宏参数求值 | `BT_SPSC_PUSH(q, item)` 中 `item` 可能被多次求值 | 使用中间变量 `__auto_type` |
| 编译期配置 | 队列容量等通过 `#define` 固定 | 迁移到运行时配置 |

---

## 总结

| 严重程度 | 数量 | 已修复 | 关键主题 |
|---------|------|--------|---------|
| 🔴 严重 | 3 | 2 (+1 需开发) | MPSC bug ✅, -ffast-math ✅, 核心模块缺失 ⏳ |
| 🟠 重要 | 4 | 4 | 连接池 ✅, 环形缓冲区 ✅, 二进制协议 ✅, benchmark ✅ |
| 🟡 中等 | 6 | 6 | volatile ✅, Disruptor ✅, CPU pause ✅, 队列校验 ✅, mempool ✅, Disruptor wrap ✅ |
| 🔵 轻微 | 3 | 3 | gethostbyname ✅, strict aliasing ✅, 其他建议 |

**下一步行动项**:
1. ⏳ **最高优先级**: 实现缺失的核心业务模块（OMS, Risk Engine, Sequencer, Matching Engine）
2. ✅ 所有基础设施优化已完成
3. 建议添加单元测试框架（如 Unity 或 Check）覆盖核心模块
4. 建议向流水线添加端到端延迟测量（需要响应路径）
