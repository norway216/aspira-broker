#ifndef BT_ORDER_GATE_H
#define BT_ORDER_GATE_H

#include "bt_types.h"
#include "bt_config.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Order Gate Layer ────────────────────────────────────────────────
 * V5: Sits between Gateway and OMS.
 *
 * Protects the core system from traffic spikes:
 *  - Backpressure control: monitors downstream queue depth
 *  - Request shaping: smooths bursty traffic
 *  - Early validation: rejects obviously invalid orders before they
 *    enter the main pipeline (symbol length, price range, etc.)
 *  - Input normalization: ensures consistent order format
 *
 * The Order Gate is intentionally lightweight — it must not become
 * a bottleneck. All checks are O(1).
 */

typedef struct bt_order_gate bt_order_gate_t;

/**
 * Create an order gate.
 * @param thread_id  Thread identifier
 * @param cpu_core   CPU core to pin to
 * @return Gate handle, or NULL on failure
 */
bt_order_gate_t *bt_order_gate_create(int thread_id, int cpu_core, int sched_id);

/**
 * Start the order gate thread.
 * @param gate       Order gate
 * @param in_queue   Input queue (from Gateway)
 * @param out_queue  Output queue (to OMS)
 * @param max_depth  Max downstream queue depth before backpressure
 * @return 0 on success
 */
int bt_order_gate_start(bt_order_gate_t *gate,
                         void *in_queue,
                         void *out_queue,
                         size_t max_depth);

/**
 * Stop the order gate thread.
 */
void bt_order_gate_stop(bt_order_gate_t *gate);

/**
 * Destroy the order gate.
 */
void bt_order_gate_destroy(bt_order_gate_t *gate);

/**
 * Get order gate statistics.
 */
void bt_order_gate_stats(const bt_order_gate_t *gate,
                          uint64_t *received, uint64_t *passed,
                          uint64_t *rejected, uint64_t *throttled);

#ifdef __cplusplus
}
#endif

#endif /* BT_ORDER_GATE_H */
