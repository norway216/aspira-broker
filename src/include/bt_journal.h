#ifndef BT_JOURNAL_H
#define BT_JOURNAL_H

#include "bt_types.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Append-Only Journal ──────────────────────────────────────────────
 *
 * Writes order and trade events to an append-only file for recovery.
 * Uses a ring buffer between the hot path and the journal I/O thread.
 * Configurable fsync batching interval for durability vs performance.
 */

typedef enum {
    BT_JOURNAL_NEW_ORDER    = 0,
    BT_JOURNAL_TRADE        = 1,
    BT_JOURNAL_CANCEL       = 2,
    BT_JOURNAL_SNAPSHOT     = 3
} bt_journal_entry_type_t;

typedef struct {
    uint64_t               seq_num;
    bt_journal_entry_type_t type;
    uint64_t               timestamp;
    union {
        bt_order_t order;
        bt_trade_t trade;
    } data;
} bt_journal_entry_t;

typedef struct bt_journal bt_journal_t;

/**
 * Open or create the journal file.
 * @param path          File path
 * @param sync_interval_ms  fsync interval (0 = no fsync, fastest)
 * @return Journal handle, or NULL on failure
 */
bt_journal_t *bt_journal_open(const char *path, int sync_interval_ms);

/**
 * Append an entry to the journal (non-blocking, lock-free write to ring buffer).
 * Returns 0 on success, -1 if ring buffer is full.
 */
int bt_journal_append(bt_journal_t *j, const bt_journal_entry_t *entry);

/**
 * Flush all pending entries to disk and fsync.
 */
void bt_journal_flush(bt_journal_t *j);

/**
 * Close the journal, flush pending data, and free resources.
 */
void bt_journal_close(bt_journal_t *j);

/**
 * Get journal statistics.
 */
void bt_journal_stats(const bt_journal_t *j, uint64_t *written, uint64_t *dropped);

#ifdef __cplusplus
}
#endif

#endif /* BT_JOURNAL_H */
