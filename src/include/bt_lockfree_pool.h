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
 * FIX (2026-06): Replaced `*(void **)ptr` strict-aliasing violation with
 * an explicit `bt_lfpool_node_t` header that objects must embed at offset 0.
 * This is both C11-compliant and clearer about the ownership contract.
 */

/* Pooled objects must embed this header at offset 0 */
typedef struct bt_lfpool_node {
    struct bt_lfpool_node *next;
} bt_lfpool_node_t;

typedef struct bt_lfpool {
    _Atomic bt_lfpool_node_t *head;  /* CAS-guarded freelist head */
    _Atomic size_t alloc_count;      /* number of allocs (for stats) */
    _Atomic size_t free_count;       /* number of frees (for stats) */
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
 *
 * Usage: `MyObj *obj = (MyObj *)bt_lfpool_alloc(&pool);`
 *   The first field of MyObj must be `bt_lfpool_node_t _pool_node;`.
 */
static inline void *bt_lfpool_alloc(bt_lfpool_t *pool)
{
    bt_lfpool_node_t *head;
    do {
        head = (bt_lfpool_node_t *)__atomic_load_n(&pool->head, __ATOMIC_ACQUIRE);
        if (!head) {
            return NULL; /* pool empty */
        }
    } while (!__atomic_compare_exchange_n(&pool->head, &head,
                 head->next, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));

    __atomic_fetch_add(&pool->alloc_count, 1, __ATOMIC_RELAXED);
    return head;
}

/**
 * Return an object to the pool's free list.
 * Thread-safe: multiple threads can free concurrently.
 *
 * @param ptr  Pointer to an object whose first field is bt_lfpool_node_t.
 */
static inline void bt_lfpool_free(bt_lfpool_t *pool, void *ptr)
{
    if (!ptr) return;

    bt_lfpool_node_t *node = (bt_lfpool_node_t *)ptr;
    bt_lfpool_node_t *old_head = (bt_lfpool_node_t *)__atomic_load_n(&pool->head, __ATOMIC_ACQUIRE);
    do {
        node->next = old_head;
    } while (!__atomic_compare_exchange_n(&pool->head, &old_head, node,
                 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));

    __atomic_fetch_add(&pool->free_count, 1, __ATOMIC_RELAXED);
}

/**
 * Bulk push: add an array of objects to the pool.
 * More efficient than individual frees for batch operations.
 */
static inline void bt_lfpool_bulk_free(bt_lfpool_t *pool, void **ptrs, size_t count)
{
    if (!count) return;

    /* Link the array into a chain using explicit node pointers */
    for (size_t i = 0; i < count - 1; i++) {
        ((bt_lfpool_node_t *)ptrs[i])->next = (bt_lfpool_node_t *)ptrs[i + 1];
    }

    bt_lfpool_node_t *old_head = (bt_lfpool_node_t *)__atomic_load_n(&pool->head, __ATOMIC_ACQUIRE);
    do {
        ((bt_lfpool_node_t *)ptrs[count - 1])->next = old_head;
    } while (!__atomic_compare_exchange_n(&pool->head, &old_head,
                 (bt_lfpool_node_t *)ptrs[0], 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));

    __atomic_fetch_add(&pool->free_count, count, __ATOMIC_RELAXED);
}

/**
 * Get pool statistics.
 */
static inline void bt_lfpool_stats(const bt_lfpool_t *pool,
                                    size_t *allocs, size_t *frees)
{
    if (allocs) *allocs = __atomic_load_n(&pool->alloc_count, __ATOMIC_RELAXED);
    if (frees)  *frees  = __atomic_load_n(&pool->free_count, __ATOMIC_RELAXED);
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
