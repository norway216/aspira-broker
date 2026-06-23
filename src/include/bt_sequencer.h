#ifndef BT_SEQUENCER_H
#define BT_SEQUENCER_H

#include "bt_types.h"
#include "bt_config.h"
#include "bt_journal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sequencer ────────────────────────────────────────────────────────
 * The Sequencer sits between the Risk Engine and Matching Engine.
 * It assigns a deterministic global sequence ID to each order,
 * ensuring total ordering across all matching shards.
 *
 * This is critical for:
 *  - Deterministic replay (same input → same output)
 *  - Snapshot + journal recovery
 *  - Audit trail generation
 *
 * Input:  bt_risk_result_t + bt_order_request_t (from Risk Engine)
 * Output: stamped bt_order_request_t (to Matching Engine shard)
 */

/* ── Sequencer context ───────────────────────────────────────────────
 * Queue element types are defined in bt_queues.h:
 *   bt_risk_seq_msg_t  — Risk → Sequencer (input)
 *   bt_seq_match_msg_t — Sequencer → Matching (output)
 */
typedef struct bt_sequencer bt_sequencer_t;

/**
 * Create a sequencer instance.
 * @param thread_id   Thread identifier
 * @param cpu_core    CPU core to pin to
 * @return Sequencer handle, or NULL on failure
 */
bt_sequencer_t *bt_sequencer_create(int thread_id, int cpu_core, int sched_id, bt_journal_t *journal);

/**
 * Start the sequencer thread.
 * @param seq        Sequencer handle
 * @param in_queue   Input queue (from Risk Engine)
 * @param out_queues Array of output queues (one per Matching Engine shard)
 * @param num_shards Number of matching engine shards
 * @return 0 on success
 */
int bt_sequencer_start(bt_sequencer_t *seq,
                        void *in_queue,
                        void **out_queues,
                        int num_shards);

/**
 * Stop the sequencer thread and wait for completion.
 */
void bt_sequencer_stop(bt_sequencer_t *seq);

/**
 * Destroy the sequencer and free resources.
 */
void bt_sequencer_destroy(bt_sequencer_t *seq);

/**
 * Get the current global sequence number.
 */
uint64_t bt_sequencer_get_global_seq(const bt_sequencer_t *seq);

/**
 * Get sequencer statistics.
 */
void bt_sequencer_stats(const bt_sequencer_t *seq,
                         uint64_t *orders_processed,
                         uint64_t *global_seq);

#ifdef __cplusplus
}
#endif

#endif /* BT_SEQUENCER_H */
