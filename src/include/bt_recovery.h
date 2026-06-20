#ifndef BT_RECOVERY_H
#define BT_RECOVERY_H

#include "bt_types.h"
#include "bt_journal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Journal Replay / Crash Recovery ───────────────────────────────────
 * Reconstructs system state from the append-only journal on restart. */

/**
 * Attempt recovery from journal.
 * @param journal_path  Path to the journal file
 * @param global_seq    Output: last global sequence number from journal
 * @param total_orders  Output: number of orders replayed
 * @param total_trades  Output: number of trades replayed
 * @return 0 on success, -1 if journal is missing/unreadable, 1 if empty
 */
int bt_recovery_replay(const char *journal_path,
                        uint64_t *global_seq,
                        uint64_t *total_orders,
                        uint64_t *total_trades);

#ifdef __cplusplus
}
#endif

#endif /* BT_RECOVERY_H */
