#include "bt_memory_pool.h"
#include "bt_cpu.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Memory Pool Implementation ─────────────────────────────────────── */

int bt_mempool_init(bt_mempool_t *pool, size_t total_size,
                    int num_arenas, int use_hugepages)
{
    if (!pool || total_size == 0 || num_arenas < 1) return -1;

    memset(pool, 0, sizeof(*pool));

    /* Allocate the main slab */
    if (use_hugepages) {
        pool->base = bt_cpu_alloc_hugepages(total_size);
    } else {
        /* Use aligned alloc for regular pages */
        if (posix_memalign(&pool->base, 2 * 1024 * 1024, total_size) != 0) {
            pool->base = NULL;
        }
    }

    if (!pool->base) {
        fprintf(stderr, "bt_mempool_init: failed to allocate %zu MB\n",
                total_size / (1024 * 1024));
        return -1;
    }

    /* Prefault pages to avoid page faults in hot path */
    bt_cpu_prefault(pool->base, total_size);
    bt_cpu_madvise_hugepage(pool->base, total_size);

    pool->total_size = total_size;
    pool->num_arenas = num_arenas;
    pool->hugepage = use_hugepages;
    atomic_init(&pool->offset, 0);

    /* Arena metadata at end of slab */
    size_t arena_array_size = num_arenas * sizeof(bt_mempool_arena_t);
    size_t usable_size = total_size - arena_array_size;
    size_t arena_size = usable_size / num_arenas;

    /* Align arena size to page boundary */
    arena_size &= ~(size_t)(4095);

    pool->arenas = (uint8_t*)pool->base + usable_size;

    /* Initialize arenas */
    bt_mempool_arena_t *arenas = (bt_mempool_arena_t *)pool->arenas;
    uint8_t *arena_base = (uint8_t *)pool->base;

    for (int i = 0; i < num_arenas; i++) {
        memset(&arenas[i], 0, sizeof(bt_mempool_arena_t));
        arenas[i].base  = arena_base + (i * arena_size);
        arenas[i].size  = arena_size;
        arenas[i].offset = 0;
        arenas[i].free_orders = NULL;
        arenas[i].alloc_count = 0;
        arenas[i].free_count  = 0;
    }

    return 0;
}

void bt_mempool_destroy(bt_mempool_t *pool)
{
    if (!pool || !pool->base) return;

    if (pool->hugepage) {
        bt_cpu_free_hugepages(pool->base, pool->total_size);
    } else {
        free(pool->base);
    }
    memset(pool, 0, sizeof(*pool));
}

/* TLS arena cache — used after first assignment per thread */
static __thread bt_mempool_arena_t *tls_arena = NULL;

bt_mempool_arena_t *bt_mempool_get_arena(bt_mempool_t *pool)
{
    /* Return cached arena if already assigned for this thread */
    if (tls_arena) return tls_arena;

    /* Assign a new arena using an atomic counter (round-robin) */
    unsigned int id = atomic_fetch_add(&pool->offset, 1) % pool->num_arenas;
    bt_mempool_arena_t *arenas = (bt_mempool_arena_t *)pool->arenas;
    tls_arena = &arenas[id];
    return tls_arena;
}

/* Allocate a fresh arena WITHOUT TLS caching — for cross-thread assignment.
 * Used by the main thread to assign dedicated arenas to worker threads. */
bt_mempool_arena_t *bt_mempool_assign_arena(bt_mempool_t *pool)
{
    unsigned int id = atomic_fetch_add(&pool->offset, 1) % pool->num_arenas;
    bt_mempool_arena_t *arenas = (bt_mempool_arena_t *)pool->arenas;
    return &arenas[id];
}

void *bt_mempool_alloc(bt_mempool_arena_t *arena, size_t size)
{
    if (!arena) return NULL;

    /* Align to cache line */
    size = (size + BT_CACHE_LINE_SIZE - 1) & ~(size_t)(BT_CACHE_LINE_SIZE - 1);

    if (arena->offset + size > arena->size) {
        return NULL; /* arena exhausted */
    }

    void *ptr = arena->base + arena->offset;
    arena->offset += size;
    arena->alloc_count++;
    return ptr;
}

bt_order_node_t *bt_mempool_alloc_order(bt_mempool_arena_t *arena)
{
    if (!arena) return NULL;

    /* Check free list first */
    if (arena->free_orders) {
        bt_order_node_t *node = arena->free_orders;
        arena->free_orders = node->next;
        arena->alloc_count++;
        arena->free_count--;
        memset(node, 0, sizeof(bt_order_node_t));
        return node;
    }

    /* Allocate fresh */
    return (bt_order_node_t *)bt_mempool_alloc(arena, sizeof(bt_order_node_t));
}

void bt_mempool_free_order(bt_mempool_arena_t *arena, bt_order_node_t *node)
{
    if (!arena || !node) return;

    /* Push to thread-local free list */
    node->next = arena->free_orders;
    arena->free_orders = node;
    arena->free_count++;
}

void bt_mempool_arena_reset(bt_mempool_arena_t *arena)
{
    if (!arena) return;
    arena->offset = 0;
    arena->free_orders = NULL;
    arena->alloc_count = 0;
    arena->free_count = 0;
}

void bt_mempool_arena_stats(const bt_mempool_arena_t *arena,
                             size_t *used, size_t *total,
                             uint32_t *allocs, uint32_t *frees)
{
    if (!arena) return;
    if (used)  *used  = arena->offset;
    if (total) *total = arena->size;
    if (allocs) *allocs = arena->alloc_count;
    if (frees) *frees = arena->free_count;
}
