#ifndef BT_LOCKFREE_POOL_H
#define BT_LOCKFREE_POOL_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "bt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lock-Free Object Pool ────────────────────────────────────────────
 * CAS-based freelist for multi-producer, multi-consumer object pooling.
 * Safe to allocate and free from any thread without locks.
 *
 * Each pooled object embeds a next-pointer at offset 0.
 * The caller is responsible for the object's memory — the pool
 * only manages the free list of recycled objects.
 */

typedef struct bt_lfpool {
    _Atomic void *head;           /* CAS-guarded freelist head */
    _Atomic size_t alloc_count;   /* number of allocs (for stats) */
    _Atomic size_t free_count;    /* number of frees (for stats) */
    char _pad[BT_CACHE_LINE_SIZE - 24];
} bt_lfpool_t;

/* ── C API ─────────────────────────────────────────────────────────── */

static inline void bt_lfpool_init(bt_lfpool_t *pool)
{
    atomic_init(&pool->head, NULL);
    atomic_init(&pool->alloc_count, 0);
    atomic_init(&pool->free_count, 0);
}

/**
 * Pop an object from the pool's free list.
 * Returns NULL if the pool is empty.
 * Thread-safe: multiple threads can allocate concurrently.
 */
static inline void *bt_lfpool_alloc(bt_lfpool_t *pool)
{
    void *head;
    do {
        head = atomic_load_explicit(&pool->head, memory_order_acquire);
        if (!head) {
            /* Pool empty — caller must fall back to bulk allocation */
            return NULL;
        }
    } while (!atomic_compare_exchange_weak_explicit(
                 &pool->head, &head,
                 *(void **)head,  /* next pointer at offset 0 */
                 memory_order_release, memory_order_relaxed));

    atomic_fetch_add(&pool->alloc_count, 1);
    return head;
}

/**
 * Return an object to the pool's free list.
 * Thread-safe: multiple threads can free concurrently.
 */
static inline void bt_lfpool_free(bt_lfpool_t *pool, void *ptr)
{
    if (!ptr) return;

    void *old_head = atomic_load_explicit(&pool->head, memory_order_acquire);
    do {
        *(void **)ptr = old_head;
    } while (!atomic_compare_exchange_weak_explicit(
                 &pool->head, &old_head, ptr,
                 memory_order_release, memory_order_relaxed));

    atomic_fetch_add(&pool->free_count, 1);
}

/**
 * Bulk push: add an array of objects to the pool.
 * More efficient than individual frees for batch operations.
 */
static inline void bt_lfpool_bulk_free(bt_lfpool_t *pool, void **ptrs, size_t count)
{
    if (!count) return;

    /* Link the array into a chain */
    for (size_t i = 0; i < count - 1; i++) {
        *(void **)ptrs[i] = ptrs[i + 1];
    }

    void *old_head = atomic_load_explicit(&pool->head, memory_order_acquire);
    do {
        *(void **)ptrs[count - 1] = old_head;
    } while (!atomic_compare_exchange_weak_explicit(
                 &pool->head, &old_head, ptrs[0],
                 memory_order_release, memory_order_relaxed));

    atomic_fetch_add(&pool->free_count, count);
}

/**
 * Get pool statistics.
 */
static inline void bt_lfpool_stats(const bt_lfpool_t *pool,
                                    size_t *allocs, size_t *frees)
{
    if (allocs) *allocs = atomic_load(&pool->alloc_count);
    if (frees)  *frees  = atomic_load(&pool->free_count);
}

#ifdef __cplusplus
}

/* ── C++ Template Wrapper ──────────────────────────────────────────── */
#include <atomic>

template<typename T>
class LockFreePool {
    static_assert(sizeof(T) >= sizeof(void*),
                  "T must be at least pointer-sized for embedded freelist");

    struct Node {
        T data;
        Node *next;
    };

    std::atomic<Node*> head_{nullptr};
    std::atomic<size_t> alloc_count_{0};
    std::atomic<size_t> free_count_{0};

public:
    LockFreePool() = default;

    T *allocate() {
        Node *node = head_.load(std::memory_order_acquire);
        while (node) {
            if (head_.compare_exchange_weak(node, node->next,
                    std::memory_order_release, std::memory_order_relaxed)) {
                alloc_count_.fetch_add(1, std::memory_order_relaxed);
                return &node->data;
            }
        }
        return nullptr; /* pool empty */
    }

    void release(T *ptr) {
        if (!ptr) return;
        /* pointer arithmetic: Node is at T's address (T is first member) */
        Node *node = reinterpret_cast<Node*>(ptr);
        Node *old = head_.load(std::memory_order_acquire);
        do {
            node->next = old;
        } while (!head_.compare_exchange_weak(old, node,
                  std::memory_order_release, std::memory_order_relaxed));
        free_count_.fetch_add(1, std::memory_order_relaxed);
    }

    size_t allocs() const { return alloc_count_.load(std::memory_order_relaxed); }
    size_t frees()  const { return free_count_.load(std::memory_order_relaxed); }
};

#endif /* __cplusplus */
#endif /* BT_LOCKFREE_POOL_H */
