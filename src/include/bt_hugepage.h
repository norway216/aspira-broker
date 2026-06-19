#ifndef BT_HUGEPAGE_H
#define BT_HUGEPAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── HugePage Allocator ───────────────────────────────────────────────
 * Manages large contiguous allocations backed by 2MB or 1GB HugePages.
 * Used for order books, market data buffers, and historical caches.
 *
 * Simple bump-allocator semantics: alloc() bumps an offset, reset()
 * returns to the beginning. No individual free() — the entire arena
 * is recycled at once on reset.
 */

typedef struct bt_hugepage {
    void   *base;            /* mmap'd memory region */
    size_t  total_size;      /* total size in bytes */
    size_t  offset;           /* current allocation offset */
    int     use_1gb;         /* 1 = try 1GB pages, 0 = 2MB */
} bt_hugepage_t;

/**
 * Initialize a hugepage arena.
 * @param hp          Allocator struct
 * @param total_size  Desired size (will be page-aligned)
 * @param use_1gb     Try to use 1GB hugepages if available
 * @return 0 on success, -1 on failure (falls back to regular pages)
 */
int bt_hugepage_init(bt_hugepage_t *hp, size_t total_size, int use_1gb);

/**
 * Allocate memory from the hugepage arena.
 * Returns NULL if the arena is exhausted.
 * All pointers are 2MB-aligned.
 */
void *bt_hugepage_alloc(bt_hugepage_t *hp, size_t size);

/**
 * Reset the arena to its initial state.
 * All previous allocations are invalidated.
 */
void bt_hugepage_reset(bt_hugepage_t *hp);

/**
 * Destroy the hugepage arena and release memory to the OS.
 */
void bt_hugepage_destroy(bt_hugepage_t *hp);

/**
 * Get arena statistics.
 */
static inline size_t bt_hugepage_used(const bt_hugepage_t *hp)   { return hp->offset; }
static inline size_t bt_hugepage_total(const bt_hugepage_t *hp)  { return hp->total_size; }
static inline size_t bt_hugepage_avail(const bt_hugepage_t *hp)  { return hp->total_size - hp->offset; }

#ifdef __cplusplus
}

/* ── C++ Class Wrapper ─────────────────────────────────────────────── */

class HugePageAllocator {
    bt_hugepage_t hp_;
public:
    HugePageAllocator(size_t size, bool use_1gb = false) {
        bt_hugepage_init(&hp_, size, use_1gb ? 1 : 0);
    }
    ~HugePageAllocator() { bt_hugepage_destroy(&hp_); }

    void *alloc(size_t bytes)       { return bt_hugepage_alloc(&hp_, bytes); }
    void  reset()                   { bt_hugepage_reset(&hp_); }
    size_t used()  const            { return bt_hugepage_used(&hp_); }
    size_t total() const            { return bt_hugepage_total(&hp_); }
    size_t avail() const            { return bt_hugepage_avail(&hp_); }
    void *base()   const            { return hp_.base; }
};

#endif /* __cplusplus */
#endif /* BT_HUGEPAGE_H */
