#include "bt_slab_allocator.h"
#include <stdio.h>
#include <string.h>

int bt_slab_init(bt_slab_t *slab, void *memory, size_t memory_size, size_t block_size)
{
    if (!slab || !memory || memory_size == 0 || block_size == 0) return -1;

    memset(slab, 0, sizeof(*slab));

    /* Cache-line align block size */
    slab->aligned_size = (block_size + BT_CACHE_LINE_SIZE - 1) & ~(size_t)(BT_CACHE_LINE_SIZE - 1);

    if (slab->aligned_size < sizeof(void *)) {
        slab->aligned_size = sizeof(void *); /* need room for free-list pointer */
    }

    slab->num_blocks = memory_size / slab->aligned_size;
    if (slab->num_blocks == 0) return -1;

    slab->base       = memory;
    slab->block_size = block_size;
    slab->free_list  = NULL;
    slab->alloc_count = 0;
    slab->free_count  = 0;

    /* Pre-link all blocks into the free list */
    for (size_t i = 0; i < slab->num_blocks; i++) {
        void *block = (uint8_t *)slab->base + i * slab->aligned_size;
        *(void **)block = slab->free_list;
        slab->free_list = block;
        slab->free_count++;
    }

    return 0;
}

void *bt_slab_alloc(bt_slab_t *slab)
{
    if (!slab) return NULL;

    /* Thread-safe: CAS-loop on free_list head (ABA-safe since each
     * block is only ever pushed back by the thread that owns it via
     * bt_slab_free; and since slabs are NOT shared between threads
     * in Tier, we can keep this simple. For multi-thread Tier use,
     * we use __atomic CAS. */
    void *old_head, *new_head;
    do {
        old_head = (void *)__atomic_load_n(&slab->free_list, __ATOMIC_ACQUIRE);
        if (!old_head) return NULL;
        new_head = *(void **)old_head;
    } while (!__atomic_compare_exchange_n(&slab->free_list, &old_head, new_head,
                                           0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));

    __atomic_fetch_add(&slab->alloc_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&slab->free_count, 1, __ATOMIC_RELAXED);
    return old_head;
}

void bt_slab_free(bt_slab_t *slab, void *ptr)
{
    if (!slab || !ptr) return;

    /* Sanity check: is ptr within our slab? */
    if ((uintptr_t)ptr < (uintptr_t)slab->base ||
        (uintptr_t)ptr >= (uintptr_t)slab->base + slab->num_blocks * slab->aligned_size) {
        return;
    }

    /* Thread-safe: CAS-loop push onto free_list */
    void *old_head;
    do {
        old_head = (void *)__atomic_load_n(&slab->free_list, __ATOMIC_ACQUIRE);
        *(void **)ptr = old_head;
    } while (!__atomic_compare_exchange_n(&slab->free_list, &old_head, ptr,
                                           0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));

    __atomic_fetch_add(&slab->free_count, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&slab->alloc_count, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&slab->alloc_count, 1, __ATOMIC_RELAXED);
}

void bt_slab_reset(bt_slab_t *slab)
{
    if (!slab) return;
    slab->free_list = NULL;
    slab->alloc_count = 0;
    slab->free_count = 0;

    /* Re-link all blocks */
    for (size_t i = 0; i < slab->num_blocks; i++) {
        void *block = (uint8_t *)slab->base + i * slab->aligned_size;
        *(void **)block = slab->free_list;
        slab->free_list = block;
        slab->free_count++;
    }
}

void bt_slab_stats(const bt_slab_t *slab, size_t *used, size_t *total,
                    uint32_t *allocs, uint32_t *frees)
{
    if (!slab) return;
    if (used)  *used  = slab->alloc_count * slab->aligned_size;
    if (total) *total = slab->num_blocks * slab->aligned_size;
    if (allocs) *allocs = slab->alloc_count;
    if (frees)  *frees  = slab->free_count;
}
