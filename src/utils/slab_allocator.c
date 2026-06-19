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
    if (!slab || !slab->free_list) return NULL;

    void *ptr = slab->free_list;
    slab->free_list = *(void **)ptr;
    slab->alloc_count++;
    slab->free_count--;
    return ptr;
}

void bt_slab_free(bt_slab_t *slab, void *ptr)
{
    if (!slab || !ptr) return;

    /* Sanity check: is ptr within our slab? */
    if ((uintptr_t)ptr < (uintptr_t)slab->base ||
        (uintptr_t)ptr >= (uintptr_t)slab->base + slab->num_blocks * slab->aligned_size) {
        return;
    }

    *(void **)ptr = slab->free_list;
    slab->free_list = ptr;
    slab->free_count++;
    if (slab->alloc_count > 0) slab->alloc_count--;
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
