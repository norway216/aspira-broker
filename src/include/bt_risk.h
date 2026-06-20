#ifndef BT_RISK_H
#define BT_RISK_H

#include "bt_types.h"
#include "bt_queues.h"
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Pre-Trade Risk Engine ──────────────────────────────────────────────
 * Shared state + per-worker threads. */

#define BT_RISK_MAX_POSITIONS 4096

typedef struct {
    char     symbol[16];
    uint64_t user_id;
    int64_t  position;
    _Atomic double user_notional;   /* per-user exposure (Round 4) */
} bt_risk_position_t;

#define BT_RISK_MAX_USER_EXPOSURES 4096

typedef struct {
    uint64_t        user_id;
    _Atomic double  notional;        /* per-user notional */
} bt_risk_user_exposure_t;

typedef struct bt_risk_state {
    int                  kill_switch;       /* __atomic_* access */
    int                  breaker_active;    /* circuit breaker tripped? */
    uint64_t             total_checked;     /* __atomic_* access */
    uint64_t             total_passed;      /* __atomic_* access */
    uint64_t             total_rejected;    /* __atomic_* access */
    _Atomic double       total_notional;    /* C11 atomic access */
    /* Circuit breaker rate tracking (Round 4) */
    _Atomic uint64_t     rate_bucket_count;
    _Atomic uint64_t     rate_window_start_ns;
    /* Per-user exposure tracking (Round 4) */
    bt_risk_user_exposure_t *user_exposures;
    int                  num_user_exposures;
    int                  max_user_exposures;
    /* Per-symbol positions */
    bt_risk_position_t  *positions;
    int                  num_positions;
    int                  max_positions;
} bt_risk_state_t;

typedef struct bt_risk_worker {
    int                  wid;
    int                  cpu_core;
    int                  running;           /* __atomic_* access */
    bt_risk_in_queue_t  *in_queue;
    bt_seq_in_queue_t   *out_queue;
    bt_risk_state_t     *state;
    int                  nout;
    pthread_t            thread;
    uint64_t             orders_processed;  /* __atomic_* access */
} bt_risk_worker_t;

bt_risk_state_t  *bt_risk_state_create(void);
void              bt_risk_state_destroy(bt_risk_state_t *s);
void              bt_risk_kill_switch(bt_risk_state_t *s, int on);

bt_risk_worker_t *bt_risk_worker_create(int wid, int cpu,
                                         bt_risk_in_queue_t *in,
                                         bt_seq_in_queue_t *out,
                                         int nout,
                                         bt_risk_state_t *st);
int  bt_risk_worker_start(bt_risk_worker_t *w);
void bt_risk_worker_stop(bt_risk_worker_t *w);
void bt_risk_worker_destroy(bt_risk_worker_t *w);

#ifdef __cplusplus
}
#endif

#endif /* BT_RISK_H */
