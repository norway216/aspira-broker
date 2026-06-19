#ifndef BT_SLAB_ALLOCATOR_H
#define BT_SLAB_ALLOCATOR_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "bt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Slab Allocator ───────────────────────────────────────────────────
 * Fixed-size block allocation from a pre-allocated slab.
 * Per-thread: single owner, no locks. O(1) allocate and deallocate.
 *
 * Layout: [Block 0][Block 1]...[Block N-1]
 * Free blocks are linked via an embedded free list.
 */

typedef struct bt_slab {
    void        *base;           /* start of memory block */
    size_t       block_size;     /* size of each block (cache-line aligned) */
    size_t       num_blocks;     /* total number of blocks */
    size_t       aligned_size;   /* block_size rounded up to cache line */
    void        *free_list;      /* head of embedded free list */
    uint32_t     alloc_count;    /* number of blocks currently allocated */
    uint32_t     free_count;     /* number of blocks currently free */
    char         _pad[BT_CACHE_LINE_SIZE - 40]; /* pad to cache line */
} bt_slab_t;

/* ── C API ─────────────────────────────────────────────────────────── */

/**
 * Initialize a slab allocator over an existing memory region.
 * @param slab        Allocator struct (caller-allocated)
 * @param memory      Pre-allocated memory block
 * @param memory_size Total size of the memory block
 * @param block_size  Size of each fixed-size block
 * @return 0 on success, -1 if memory too small for any blocks
 */
int bt_slab_init(bt_slab_t *slab, void *memory, size_t memory_size, size_t block_size);

/**
 * Allocate a block from the slab.
 * Returns NULL if the slab is exhausted.
 */
void *bt_slab_alloc(bt_slab_t *slab);

/**
 * Return a block to the slab (O(1), lock-free per-thread).
 */
void bt_slab_free(bt_slab_t *slab, void *ptr);

/**
 * Reset the slab to its initial state (all blocks free).
 */
void bt_slab_reset(bt_slab_t *slab);

/**
 * Get slab usage statistics.
 */
void bt_slab_stats(const bt_slab_t *slab, size_t *used, size_t *total,
                    uint32_t *allocs, uint32_t *frees);

#ifdef __cplusplus
}

/* ── C++ Template Wrapper ──────────────────────────────────────────── */
#include <atomic>
#include <new>

template<size_t BlockSize, size_t N>
class SlabAllocator {
    static constexpr size_t AlignedBlockSize =
        (BlockSize + BT_CACHE_LINE_SIZE - 1) & ~(size_t)(BT_CACHE_LINE_SIZE - 1);

    alignas(BT_CACHE_LINE_SIZE) char pool_[AlignedBlockSize * N];
    std::atomic<size_t> free_count_{N};
    void *free_list_{nullptr};

public:
    SlabAllocator() { reset(); }

    void *allocate() {
        /* Try free list first */
        if (free_list_) {
            void *ptr = free_list_;
            free_list_ = *(void **)free_list_;
            free_count_.fetch_sub(1, std::memory_order_relaxed);
            return ptr;
        }
        /* Bump allocate */
        size_t idx = free_count_.fetch_sub(1, std::memory_order_relaxed);
        if (idx == 0 || idx > N) return nullptr; /* exhausted */
        size_t actual_idx = idx - 1;
        return pool_ + actual_idx * AlignedBlockSize;
    }

    void deallocate(void *ptr) {
        if (!ptr) return;
        *(void **)ptr = free_list_;
        free_list_ = ptr;
        free_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void reset() {
        free_count_.store(N, std::memory_order_relaxed);
        free_list_ = nullptr;
        /* Pre-link all blocks */
        for (size_t i = 0; i < N; i++) {
            deallocate(pool_ + i * AlignedBlockSize);
        }
    }

    size_t available() const { return free_count_.load(std::memory_order_relaxed); }
    size_t capacity()   const { return N; }
};

#endif /* __cplusplus */
#endif /* BT_SLAB_ALLOCATOR_H */
