#ifndef BT_LOCKFREE_QUEUE_H
#define BT_LOCKFREE_QUEUE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "bt_types.h"

/* ── C / C++ compatibility layer for atomics ───────────────────────── */
#ifdef __cplusplus
#include <atomic>
#define BT_ATOMIC_SZ       std::atomic<size_t>
#define BT_ALIGNAS(x)      alignas(x)
#define BT_ATOMIC_INIT(v, val)   (v).store(val, std::memory_order_relaxed)
#define BT_ATOMIC_LOAD(v, ord)   (v).load(std::memory_order_##ord)
#define BT_ATOMIC_STORE(v, val, ord) (v).store(val, std::memory_order_##ord)
#define BT_ATOMIC_CAS_WEAK(v, exp, des, succ, fail) \
    (v).compare_exchange_weak(exp, des, std::memory_order_##succ, std::memory_order_##fail)
#define BT_ATOMIC_FETCH_ADD(v, val) (v).fetch_add(val, std::memory_order_relaxed)
extern "C" {
#else
#define BT_ATOMIC_SZ       _Atomic size_t
#define BT_ALIGNAS(x)      _Alignas(x)
#define BT_ATOMIC_INIT(v, val)        atomic_init(&(v), val)
#define BT_ATOMIC_LOAD(v, ord)        atomic_load_explicit(&(v), memory_order_##ord)
#define BT_ATOMIC_STORE(v, val, ord)  atomic_store_explicit(&(v), val, memory_order_##ord)
#define BT_ATOMIC_CAS_WEAK(v, exp, des, succ, fail) \
    atomic_compare_exchange_weak_explicit(&(v), &(exp), des, memory_order_##succ, memory_order_##fail)
#define BT_ATOMIC_FETCH_ADD(v, val)   atomic_fetch_add(&(v), val)
#endif

/* ── SPSC Queue ───────────────────────────────────────────────────── */
#define BT_SPSC_QUEUE_DEF(name, type, capacity_)                         \
    typedef struct name {                                                \
        BT_ALIGNAS(BT_CACHE_LINE_SIZE) BT_ATOMIC_SZ head;              \
        BT_ALIGNAS(BT_CACHE_LINE_SIZE) BT_ATOMIC_SZ tail;              \
        BT_ALIGNAS(BT_CACHE_LINE_SIZE) type buffer[capacity_];          \
        size_t mask;                                                     \
    } name

#define BT_SPSC_QUEUE_INIT(q) do {                                       \
        BT_ATOMIC_INIT((q).head, 0);                                     \
        BT_ATOMIC_INIT((q).tail, 0);                                     \
        (q).mask = (sizeof((q).buffer)/sizeof((q).buffer[0])) - 1;      \
    } while(0)

#define BT_SPSC_QUEUE_CAP(q) ((q).mask + 1)

#define BT_SPSC_PUSH(q, item) ({                                         \
        int __ret = 0;                                                   \
        size_t __h = BT_ATOMIC_LOAD((q).head, acquire);                 \
        size_t __t = BT_ATOMIC_LOAD((q).tail, relaxed);                 \
        size_t __next = (__t + 1) & (q).mask;                           \
        if (__next != __h) {                                             \
            (q).buffer[__t] = (item);                                    \
            BT_ATOMIC_STORE((q).tail, __next, release);                 \
            __ret = 1;                                                   \
        }                                                                \
        __ret;                                                           \
    })

#define BT_SPSC_POP(q, out) ({                                           \
        int __ret = 0;                                                   \
        size_t __h = BT_ATOMIC_LOAD((q).head, relaxed);                 \
        size_t __t = BT_ATOMIC_LOAD((q).tail, acquire);                 \
        if (__h != __t) {                                                \
            *(out) = (q).buffer[__h];                                    \
            BT_ATOMIC_STORE((q).head, (__h + 1) & (q).mask, release);  \
            __ret = 1;                                                   \
        }                                                                \
        __ret;                                                           \
    })

/* ── MPSC Queue ───────────────────────────────────────────────────── */
#define BT_MPSC_QUEUE_DEF(name, type, capacity_)                         \
    typedef struct name {                                                \
        BT_ALIGNAS(BT_CACHE_LINE_SIZE) BT_ATOMIC_SZ head;              \
        BT_ALIGNAS(BT_CACHE_LINE_SIZE) BT_ATOMIC_SZ tail;              \
        BT_ALIGNAS(BT_CACHE_LINE_SIZE) type buffer[capacity_];          \
        size_t mask;                                                     \
    } name

#define BT_MPSC_QUEUE_INIT(q) do {                                       \
        BT_ATOMIC_INIT((q).head, 0);                                     \
        BT_ATOMIC_INIT((q).tail, 0);                                     \
        (q).mask = (sizeof((q).buffer)/sizeof((q).buffer[0])) - 1;      \
    } while(0)

#define BT_MPSC_QUEUE_CAP(q) ((q).mask + 1)

/* MPSC Push (Vyukov algorithm, corrected):
 * CAS-loop on tail to claim a slot. The full-check (next == head) is done
 * before CAS; if CAS succeeds despite a concurrent consumer advancing head,
 * the slot is still ours — write unconditionally.
 *
 * FIX (2026-06): the post-CAS `if (__next != head)` guard was removed.
 * After CAS won the slot, refusing to write leaves tail advanced over
 * stale/garbage data — the consumer will read it as a valid item.
 * CAS-claim + unconditional write is the canonical Vyukov MPSC form. */
#define BT_MPSC_PUSH(q, item) ({                                         \
        int __ret = 0;                                                   \
        size_t __t, __next;                                              \
        do {                                                             \
            __t = BT_ATOMIC_LOAD((q).tail, relaxed);                    \
            __next = (__t + 1) & (q).mask;                               \
            if (__next == BT_ATOMIC_LOAD((q).head, acquire)) {          \
                __ret = 0; goto __mpsc_push_done;                        \
            }                                                            \
        } while (!BT_ATOMIC_CAS_WEAK((q).tail, __t, __next, release, relaxed)); \
        (q).buffer[__t] = (item);  /* write unconditionally after CAS */ \
        __ret = 1;                                                       \
        __mpsc_push_done: ;                                              \
        __ret;                                                           \
    })

#define BT_MPSC_POP(q, out) ({                                           \
        int __ret = 0;                                                   \
        size_t __h = BT_ATOMIC_LOAD((q).head, relaxed);                 \
        size_t __t = BT_ATOMIC_LOAD((q).tail, acquire);                 \
        if (__h != __t) {                                                \
            *(out) = (q).buffer[__h];                                    \
            BT_ATOMIC_STORE((q).head, (__h + 1) & (q).mask, release);  \
            __ret = 1;                                                   \
        }                                                                \
        __ret;                                                           \
    })

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ── C++ Template Queue Classes (outside extern "C") ───────────────── */
#ifdef __cplusplus

template<typename T, size_t N>
class SPSCQueue {
    static_assert((N & (N - 1)) == 0, "N must be power of 2");
    alignas(BT_CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(BT_CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
    alignas(BT_CACHE_LINE_SIZE) T buffer_[N];
    const size_t mask_{N - 1};
public:
    bool push(const T& item) {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t next = (t + 1) & mask_;
        if (next == h) return false;
        buffer_[t] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& item) {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);
        if (h == t) return false;
        item = buffer_[h];
        head_.store((h + 1) & mask_, std::memory_order_release);
        return true;
    }
    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }
};

template<typename T, size_t N>
class MPSCQueue {
    static_assert((N & (N - 1)) == 0, "N must be power of 2");
    alignas(BT_CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(BT_CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
    alignas(BT_CACHE_LINE_SIZE) T buffer_[N];
    const size_t mask_{N - 1};
public:
    bool push(const T& item) {
        size_t t, next;
        do {
            t = tail_.load(std::memory_order_relaxed);
            next = (t + 1) & mask_;
            size_t h = head_.load(std::memory_order_acquire);
            if (next == h) return false;
        } while (!tail_.compare_exchange_weak(t, next,
                  std::memory_order_release, std::memory_order_relaxed));
        buffer_[t] = item;
        return true;
    }
    bool pop(T& item) {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);
        if (h == t) return false;
        item = buffer_[h];
        head_.store((h + 1) & mask_, std::memory_order_release);
        return true;
    }
    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }
};

#endif /* __cplusplus */
#endif /* BT_LOCKFREE_QUEUE_H */
