#ifndef BT_MEMORY_POOL_H
#define BT_MEMORY_POOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#ifdef __cplusplus
#include <atomic>
#endif
#include "bt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Memory Pool ───────────────────────────────────────────────────────
 * Pre-allocated slab allocator. No malloc/free in the hot path.
 * Uses thread-local bump allocators for lock-free allocation.
 * Supports object recycling via per-thread free lists.
 */

/* Cross-language atomic: same size/layout as size_t on x86_64 */
#ifdef __cplusplus
typedef std::atomic<size_t> bt_atomic_size_t;
#else
typedef _Atomic size_t bt_atomic_size_t;
#endif

typedef struct bt_mempool {
    void       *base;             /* start of the memory slab */
    size_t      total_size;       /* total size of the slab */
    bt_atomic_size_t offset;      /* global bump offset for bulk alloc */
    int         num_arenas;       /* number of per-thread arenas */
    void       *arenas;           /* array of bt_mempool_arena */
    int         hugepage;         /* 1 if backed by hugepages */
} bt_mempool_t;

/* ── Per-thread arena (thread-local) ───────────────────────────────── */
typedef struct bt_mempool_arena {
    uint8_t    *base;
    size_t      size;
    size_t      offset;           /* local bump offset (not atomic, single-thread) */
    bt_order_node_t *free_orders; /* freelist of recycled order nodes */
    uint32_t    alloc_count;
    uint32_t    free_count;
    char        _pad[24]; /* pad to 64 bytes cache line (40 + 24 = 64) */
} bt_mempool_arena_t;

#ifdef __cplusplus
static_assert(sizeof(bt_mempool_arena_t) % BT_CACHE_LINE_SIZE == 0,
              "arena must be cache-line aligned");
#else
_Static_assert(sizeof(bt_mempool_arena_t) % BT_CACHE_LINE_SIZE == 0,
               "arena must be cache-line aligned");
#endif

/* ── API ───────────────────────────────────────────────────────────── */

/**
 * Initialize the memory pool.
 * @param pool         Pointer to pool struct (caller-allocated)
 * @param total_size   Total size in bytes for the memory slab
 * @param num_arenas   Number of per-thread arenas to carve out
 * @param use_hugepages 1 to use HugePages, 0 for regular malloc
 * @return 0 on success, -1 on failure
 */
int bt_mempool_init(bt_mempool_t *pool, size_t total_size,
                    int num_arenas, int use_hugepages);

/**
 * Destroy the memory pool and free backing memory.
 */
void bt_mempool_destroy(bt_mempool_t *pool);

/**
 * Get the per-thread arena for the calling thread (TLS cached).
 * Creates one on first access per thread.
 */
bt_mempool_arena_t *bt_mempool_get_arena(bt_mempool_t *pool);

/**
 * Assign a new dedicated arena (NO TLS caching).
 * Use for cross-thread assignment where the caller passes the arena
 * to a worker thread. Each call returns a different arena.
 */
bt_mempool_arena_t *bt_mempool_assign_arena(bt_mempool_t *pool);

/**
 * Allocate memory from a thread-local arena (bump allocator).
 * @param arena  Thread-local arena
 * @param size   Size in bytes (will be cache-line aligned)
 * @return pointer, or NULL if arena exhausted
 */
void *bt_mempool_alloc(bt_mempool_arena_t *arena, size_t size);

/**
 * Allocate an order node from the arena (with recycling).
 */
bt_order_node_t *bt_mempool_alloc_order(bt_mempool_arena_t *arena);

/**
 * Free an order node (adds to thread-local free list for recycling).
 */
void bt_mempool_free_order(bt_mempool_arena_t *arena, bt_order_node_t *node);

/**
 * Reset the arena (for snapshot/recovery — all allocs invalidated).
 */
void bt_mempool_arena_reset(bt_mempool_arena_t *arena);

/**
 * Get arena usage statistics.
 */
void bt_mempool_arena_stats(const bt_mempool_arena_t *arena,
                             size_t *used, size_t *total,
                             uint32_t *allocs, uint32_t *frees);

#ifdef __cplusplus
}
#endif

#endif /* BT_MEMORY_POOL_H */
