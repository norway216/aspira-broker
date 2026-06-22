#ifndef BT_OMS_H
#define BT_OMS_H

#include "bt_types.h"
#include "bt_queues.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Order Management System ────────────────────────────────────────────
 * Relays validated orders from Order Gate to Risk Engine. */

typedef struct bt_oms_ctx {
    int                  thread_id;
    int                  cpu_core;
    int                  running;           /* __atomic_* access */
    bt_gw_oms_queue_t   *in_queue;          /* from Order Gate */
    bt_risk_in_queue_t  *out_queue;         /* to Risk Engine */
    pthread_t            thread;
    uint64_t             orders_processed;  /* __atomic_* access */
  int sched_id;
} bt_oms_ctx_t;

bt_oms_ctx_t *bt_oms_create(int tid, int cpu,
                             bt_gw_oms_queue_t *in,
                             bt_risk_in_queue_t *out,
                             int sched_id);
int  bt_oms_start(bt_oms_ctx_t *ctx);
void bt_oms_stop(bt_oms_ctx_t *ctx);
void bt_oms_destroy(bt_oms_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* BT_OMS_H */
