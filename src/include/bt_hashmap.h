/* ── C Hash Map (uint64_t → void*) ────────────────────────────────────
 * Open-addressing with linear probing, power-of-2 capacity.
 * Preallocated at init; no heap allocation in hot path.
 *
 * Usage:
 *   bt_hashmap_t map;
 *   bt_hashmap_init(&map, 65536);
 *   bt_hashmap_put(&map, order_id, node_ptr);  // 0=ok, -1=dup/full
 *   void *val = bt_hashmap_get(&map, order_id); // NULL if not found
 *   bt_hashmap_remove(&map, order_id);
 *   bt_hashmap_destroy(&map); */

#ifndef BT_HASHMAP_H
#define BT_HASHMAP_H

#include "bt_types.h"
#include <stdlib.h>
#include <string.h>

#define BT_HASHMAP_LOAD_FACTOR_NUM  3
#define BT_HASHMAP_LOAD_FACTOR_DEN  4  /* 75% max load */
#define BT_HASHMAP_TOMBSTONE        ((void *)(uintptr_t)1)

typedef struct {
    uint64_t key;
    void    *value;
} bt_hashmap_entry_t;

typedef struct {
    bt_hashmap_entry_t *entries;
    size_t              capacity;   /* always power of 2 */
    size_t              count;      /* active entries (excl. tombstones) */
    size_t              tombstones; /* deleted entries */
    int                 owned;      /* 1 = calloc'd entries, 0 = external */
} bt_hashmap_t;

/* ── Internal: hash + probe ────────────────────────────────────────── */
static inline size_t _hm_probe(uint64_t key, size_t i, size_t cap) {
    return ((key + i) & (cap - 1));
}

/* ── Initialize with preallocated or calloc'd storage ──────────────── */
static inline int bt_hashmap_init(bt_hashmap_t *m, size_t capacity)
{
    if (!m || capacity < 8) return -1;
    /* Round up to power of 2 */
    size_t cap = 8;
    while (cap < capacity) cap <<= 1;
    m->entries = (bt_hashmap_entry_t *)calloc(cap, sizeof(bt_hashmap_entry_t));
    if (!m->entries) return -1;
    m->capacity   = cap;
    m->count      = 0;
    m->tombstones = 0;
    m->owned      = 1;
    return 0;
}

/* ── Destroy ───────────────────────────────────────────────────────── */
static inline void bt_hashmap_destroy(bt_hashmap_t *m)
{
    if (m && m->owned && m->entries) { free(m->entries); m->entries = NULL; }
    m->capacity = m->count = m->tombstones = 0;
}

/* Forward declaration */
static inline int bt_hashmap_put(bt_hashmap_t *m, uint64_t key, void *value);

/* ── Resize (internal) ──────────────────────────────────────────────── */
static inline int _hm_resize(bt_hashmap_t *m, size_t new_cap)
{
    bt_hashmap_entry_t *old = m->entries;
    size_t old_cap = m->capacity;

    m->entries = (bt_hashmap_entry_t *)calloc(new_cap, sizeof(bt_hashmap_entry_t));
    if (!m->entries) { m->entries = old; return -1; }
    m->capacity   = new_cap;
    m->count      = 0;
    m->tombstones = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].value && old[i].value != BT_HASHMAP_TOMBSTONE) {
            bt_hashmap_put(m, old[i].key, old[i].value);
        }
    }
    free(old);
    return 0;
}

/* ── Insert or update ──────────────────────────────────────────────── */
static inline int bt_hashmap_put(bt_hashmap_t *m, uint64_t key, void *value)
{
    if (!m || !m->entries || !value) return -1;

    /* Resize if load factor exceeded */
    if ((m->count + m->tombstones) * BT_HASHMAP_LOAD_FACTOR_DEN >=
        m->capacity * BT_HASHMAP_LOAD_FACTOR_NUM) {
        if (_hm_resize(m, m->capacity * 2) != 0) return -1;
    }

    for (size_t i = 0; i < m->capacity; i++) {
        size_t idx = _hm_probe(key, i, m->capacity);
        if (!m->entries[idx].value ||
            m->entries[idx].value == BT_HASHMAP_TOMBSTONE) {
            m->entries[idx].key   = key;
            m->entries[idx].value = value;
            m->count++;
            if (m->entries[idx].value == BT_HASHMAP_TOMBSTONE)
                m->tombstones--;
            return 0;
        }
        if (m->entries[idx].key == key) {
            /* Key exists — update (or reject dup) */
            return -1; /* reject duplicates */
        }
    }
    return -1; /* should not reach here */
}

/* ── Lookup ────────────────────────────────────────────────────────── */
static inline void *bt_hashmap_get(bt_hashmap_t *m, uint64_t key)
{
    if (!m || !m->entries) return NULL;
    for (size_t i = 0; i < m->capacity; i++) {
        size_t idx = _hm_probe(key, i, m->capacity);
        if (!m->entries[idx].value) return NULL;          /* empty slot */
        if (m->entries[idx].value != BT_HASHMAP_TOMBSTONE &&
            m->entries[idx].key == key)
            return m->entries[idx].value;
        /* tombstone or key mismatch → keep probing */
    }
    return NULL;
}

/* ── Remove ────────────────────────────────────────────────────────── */
static inline int bt_hashmap_remove(bt_hashmap_t *m, uint64_t key)
{
    if (!m || !m->entries) return -1;
    for (size_t i = 0; i < m->capacity; i++) {
        size_t idx = _hm_probe(key, i, m->capacity);
        if (!m->entries[idx].value) return -1;             /* not found */
        if (m->entries[idx].value != BT_HASHMAP_TOMBSTONE &&
            m->entries[idx].key == key) {
            m->entries[idx].value = BT_HASHMAP_TOMBSTONE;
            m->count--;
            m->tombstones++;
            return 0;
        }
    }
    return -1;
}

/* ── Size ──────────────────────────────────────────────────────────── */
static inline size_t bt_hashmap_size(const bt_hashmap_t *m) {
    return m ? m->count : 0;
}

/* ── Clear (keep capacity) ─────────────────────────────────────────── */
static inline void bt_hashmap_clear(bt_hashmap_t *m)
{
    if (!m || !m->entries) return;
    memset(m->entries, 0, m->capacity * sizeof(bt_hashmap_entry_t));
    m->count = m->tombstones = 0;
}

#endif /* BT_HASHMAP_H */
