#include "bt_memory_tier.h"
#include "bt_slab_allocator.h"
#include "bt_numa.h"
#include "bt_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ── V6 Memory Tiering Implementation ──────────────────────────────── */

static bt_slab_t  *g_hot_slabs   = NULL;
static int         g_num_hot     = 0;
static int         g_hot_next    = 0;
static void       *g_warm_pool   = NULL;
static size_t      g_warm_size   = 0;
static size_t      g_warm_offset = 0;

static bt_tier_stats_t g_tier_stats;

/* ── Tiered allocation ─────────────────────────────────────────────── */
void *bt_tier_alloc(bt_tier_t tier, size_t size)
{
    switch (tier) {
    case BT_TIER_HOT:
        /* Round-robin across hot slabs */
        if (g_num_hot > 0) {
            int idx = __atomic_fetch_add(&g_hot_next, 1, __ATOMIC_RELAXED) % g_num_hot;
            void *ptr = bt_slab_alloc(&g_hot_slabs[idx]);
            if (ptr) {
                __atomic_fetch_add(&g_tier_stats.hot_allocs, 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&g_tier_stats.hot_bytes, size, __ATOMIC_RELAXED);
                return ptr;
            }
        }
        /* Fallback to warm */
        fprintf(stderr, "[tier] HOT exhausted, falling back to WARM\n");
        /* FALLTHROUGH */
    case BT_TIER_WARM:
        if (g_warm_pool && g_warm_offset + size <= g_warm_size) {
            void *ptr = (uint8_t *)g_warm_pool + g_warm_offset;
            size_t aligned = (size + 63) & ~(size_t)63;
            g_warm_offset += aligned;
            __atomic_fetch_add(&g_tier_stats.warm_allocs, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_tier_stats.warm_bytes, size, __ATOMIC_RELAXED);
            return ptr;
        }
        /* Fallback to cold */
        fprintf(stderr, "[tier] WARM exhausted, falling back to COLD\n");
        /* FALLTHROUGH */
    case BT_TIER_COLD:
    default:
        __atomic_fetch_add(&g_tier_stats.cold_allocs, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_tier_stats.cold_bytes, size, __ATOMIC_RELAXED);
        return calloc(1, size);
    }
}

void bt_tier_free(bt_tier_t tier, void *ptr, size_t size)
{
    if (!ptr) return;
    switch (tier) {
    case BT_TIER_HOT:
        /* Return to hot slab — find which slab owns this ptr */
        for (int i = 0; i < g_num_hot; i++) {
            bt_slab_free(&g_hot_slabs[i], ptr);
            return;
        }
        break;
    case BT_TIER_WARM:
        /* WARM allocations are never freed individually (bump allocator) */
        break;
    case BT_TIER_COLD:
        free(ptr);
        break;
    }
    (void)size;
}

bt_tier_t bt_tier_classify(const void *ptr)
{
    if (!ptr) return BT_TIER_COLD;
    /* Check hot slabs */
    for (int i = 0; i < g_num_hot; i++) {
        uintptr_t p   = (uintptr_t)ptr;
        uintptr_t base = (uintptr_t)g_hot_slabs[i].base;
        if (p >= base && p < base + g_hot_slabs[i].num_blocks * g_hot_slabs[i].aligned_size)
            return BT_TIER_HOT;
    }
    /* Check warm pool */
    if (g_warm_pool) {
        uintptr_t p    = (uintptr_t)ptr;
        uintptr_t base = (uintptr_t)g_warm_pool;
        if (p >= base && p < base + g_warm_size)
            return BT_TIER_WARM;
    }
    return BT_TIER_COLD;
}

/* ── Global init ───────────────────────────────────────────────────── */
int bt_tier_init(size_t hot_pool_mb, size_t warm_pool_mb)
{
    memset(&g_tier_stats, 0, sizeof(g_tier_stats));

    /* HOT: multiple slab allocators for thread-local use */
    g_num_hot = 8;
    size_t hot_per_slab = (hot_pool_mb * 1024 * 1024) / g_num_hot;
    g_hot_slabs = (bt_slab_t *)calloc(g_num_hot, sizeof(bt_slab_t));
    if (!g_hot_slabs) return -1;

    for (int i = 0; i < g_num_hot; i++) {
        void *mem = bt_numa_alloc_local(hot_per_slab);
        if (!mem || bt_slab_init(&g_hot_slabs[i], mem, hot_per_slab, 64) < 0) {
            fprintf(stderr, "[tier] HOT slab %d init failed\n", i);
            return -1;
        }
    }

    /* WARM: NUMA-local mmap pool */
    g_warm_size = warm_pool_mb * 1024 * 1024;
    g_warm_pool = bt_numa_alloc_local(g_warm_size);
    if (!g_warm_pool) {
        fprintf(stderr, "[tier] WARM pool init failed\n");
        return -1;
    }
    g_warm_offset = 0;

    fprintf(stderr, "[tier] initialized: HOT=%zuMB (%d slabs) WARM=%zuMB\n",
            hot_pool_mb, g_num_hot, warm_pool_mb);
    return 0;
}

void bt_tier_stats(bt_tier_stats_t *stats)
{
    if (stats) memcpy(stats, &g_tier_stats, sizeof(g_tier_stats));
}

#ifdef BT_DEBUG_TIER
void bt_tier_check(const char *file, int line, bt_tier_t expected)
{
    /* Placeholder: in production, track current context via TLS */
    (void)file; (void)line; (void)expected;
}
#endif
