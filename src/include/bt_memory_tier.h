#ifndef BT_MEMORY_TIER_H
#define BT_MEMORY_TIER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── V6 Memory Tiering ───────────────────────────────────────────────
 * Explicit classification of memory into latency tiers.
 *
 * HOT PATH  (L1/L2 cache resident, < 100ns access)
 *   → Slab allocator, stack, thread-local data
 *   → Must be cache-line aligned, pre-allocated
 *   → ZERO dynamic allocation in hot path
 *
 * WARM PATH (L3 cache / local NUMA, < 500ns access)
 *   → Lock-free pools, shared memory IPC buffers
 *   → Pre-allocated, atomic operations safe
 *   → Allocation allowed at startup only
 *
 * COLD PATH (DRAM / disk, > 1μs access)
 *   → Heap allocations, file I/O, log buffers
 *   → Can use malloc/free
 *   → Used for: config, stats, persistence, audit
 *
 * Design rule:
 *   HOT never calls COLD. WARM can bridge HOT ↔ COLD via event bus.
 */

typedef enum {
    BT_TIER_HOT  = 0,   /* cache-line aligned, no syscalls, pre-allocated */
    BT_TIER_WARM = 1,   /* NUMA-local, atomic-safe, startup-only alloc */
    BT_TIER_COLD = 2,   /* general heap, file-backed, any-time alloc */
} bt_tier_t;

/* ── Tiered allocation API ─────────────────────────────────────────── */

/**
 * Allocate memory in the specified tier.
 * HOT:  from pre-allocated slab (must be initialized first)
 * WARM: from NUMA-local mmap region
 * COLD: regular calloc
 */
void *bt_tier_alloc(bt_tier_t tier, size_t size);

/**
 * Free memory allocated with bt_tier_alloc.
 */
void bt_tier_free(bt_tier_t tier, void *ptr, size_t size);

/**
 * Check if a pointer belongs to a specific tier.
 */
bt_tier_t bt_tier_classify(const void *ptr);

/* ── Tier enforcement ──────────────────────────────────────────────── */

/**
 * Assert that the current allocation context matches the expected tier.
 * In debug builds, logs a warning if a cold-path function allocates
 * from a hot-path pool. In release, compiled to no-op.
 */
#ifdef BT_DEBUG_TIER
#define BT_TIER_ASSERT(tier) bt_tier_check(__FILE__, __LINE__, tier)
void bt_tier_check(const char *file, int line, bt_tier_t expected);
#else
#define BT_TIER_ASSERT(tier) ((void)0)
#endif

/* ── Tiered allocator accessors ────────────────────────────────────── */

/**
 * Get the HOT tier slab allocator for the calling thread.
 * Returns NULL if no slab is assigned (must call bt_slab_init first).
 */
void *bt_tier_hot_slab(void);

/**
 * Initialize the global tiered memory system.
 * Must be called once at startup, before any allocation.
 */
int bt_tier_init(size_t hot_pool_mb, size_t warm_pool_mb);

/**
 * Get tier statistics.
 */
typedef struct {
    uint64_t hot_allocs;
    uint64_t hot_bytes;
    uint64_t warm_allocs;
    uint64_t warm_bytes;
    uint64_t cold_allocs;
    uint64_t cold_bytes;
} bt_tier_stats_t;

void bt_tier_stats(bt_tier_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* BT_MEMORY_TIER_H */
