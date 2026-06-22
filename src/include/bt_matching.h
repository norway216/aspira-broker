#ifndef BT_MATCHING_H
#define BT_MATCHING_H

#include "bt_types.h"
#include "bt_queues.h"
#include "bt_memory_pool.h"
#include "bt_journal.h"
#include "bt_event.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Matching Engine Context ────────────────────────────────────────────
 * One instance per shard. Owns per-symbol OrderBook instances.
 * Receives globally-sequenced orders, matches, publishes events.
 *
 * Fields that need cross-thread visibility (running, counters) use
 * plain types — they are accessed via __atomic_* builtins in the .cpp
 * implementation for C/C++ portability. */

typedef struct bt_matching_ctx {
    int                  thread_id;
    int                  cpu_core;
    int                  running;           /* set/read via __atomic_* */
    bt_match_in_queue_t *in_queue;          /* from Sequencer (MPSC) */
    bt_md_tick_queue_t  *md_queue;          /* to Market Data (SPSC) */
    bt_mempool_arena_t  *arena;             /* memory pool arena */
    bt_journal_t        *journal;           /* append-only journal */
    bt_event_bus_t           *event_bus;      /* V5 event bus */
    bt_gw_response_queue_t   *response_queue;  /* responses back to gateway */
    pthread_t            thread;
    uint64_t             orders_received;   /* set/read via __atomic_* */
    uint64_t             orders_matched;    /* set/read via __atomic_* */
    uint64_t             trades_generated;  /* set/read via __atomic_* */
    uint64_t             lat_sum, lat_max, lat_count; /* V7 latency stats */
    int                  sched_id;       /* V9 scheduler entry */
} bt_matching_ctx_t;

bt_matching_ctx_t *bt_matching_create(int tid, int cpu,
                                       bt_match_in_queue_t *in,
                                       bt_md_tick_queue_t *md,
                                       bt_mempool_arena_t *arena,
                                       bt_journal_t *journal,
                                       bt_event_bus_t *event_bus,
                                       bt_gw_response_queue_t *response_queue,
                                       int sched_id);
int  bt_matching_start(bt_matching_ctx_t *ctx);
void bt_matching_stop(bt_matching_ctx_t *ctx);
void bt_matching_destroy(bt_matching_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* BT_MATCHING_H */
