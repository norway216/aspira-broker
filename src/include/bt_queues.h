#ifndef BT_QUEUES_H
#define BT_QUEUES_H

#include "bt_types.h"
#include "bt_lockfree_queue.h"
#include "bt_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Shared Queue Element Types ────────────────────────────────────── */

/* Gateway → OMS */
typedef struct {
    bt_order_request_t request;
    uint64_t           seq_num;
} bt_gw_oms_msg_t;

/* OMS → Risk */
typedef struct {
    bt_order_request_t request;
    uint64_t           seq_num;
} bt_oms_risk_msg_t;

/* Risk → Matching Engine */
typedef struct {
    bt_order_request_t request;
    bt_risk_result_t   risk;
    uint64_t           seq_num;
} bt_risk_match_msg_t;

/* Matching Engine → Market Data */
BT_SPSC_QUEUE_DEF(bt_md_tick_queue_t, bt_md_tick_t, BT_CFG_MATCH_QUEUE_CAP);

/* ── Queue Type Definitions ────────────────────────────────────────── */

/* MPSC: Gateway → OMS */
BT_MPSC_QUEUE_DEF(bt_gw_oms_queue_t, bt_gw_oms_msg_t, BT_CFG_GATEWAY_QUEUE_CAP);

/* MPSC: OMS → Risk */
BT_MPSC_QUEUE_DEF(bt_oms_risk_queue_t, bt_oms_risk_msg_t, BT_CFG_OMS_QUEUE_CAP);

/* MPSC: Risk workers pull from shared input */
BT_MPSC_QUEUE_DEF(bt_risk_in_queue_t, bt_oms_risk_msg_t, BT_CFG_RISK_QUEUE_CAP);

/* MPSC: Risk → Matching Engine shard (multiple risk workers, one matcher) */
BT_MPSC_QUEUE_DEF(bt_match_in_queue_t, bt_risk_match_msg_t, BT_CFG_MATCH_QUEUE_CAP);

/* SPSC: Matching Engine → Market Data (md ticks) */
/* (bt_md_tick_queue_t defined above) */

/* ── Helper: initialize queues ─────────────────────────────────────── */

static inline void bt_queues_init_all(
    bt_gw_oms_queue_t   *gw_oms,
    bt_oms_risk_queue_t *oms_risk,
    bt_risk_in_queue_t  *risk_in,
    bt_match_in_queue_t *match_in,     /* array of num_matchers, MPSC */
    bt_md_tick_queue_t  *md_tick,     /* array of num_matchers, SPSC */
    int num_matchers)
{
    BT_MPSC_QUEUE_INIT(*gw_oms);
    BT_MPSC_QUEUE_INIT(*oms_risk);
    BT_MPSC_QUEUE_INIT(*risk_in);

    for (int i = 0; i < num_matchers; i++) {
        BT_MPSC_QUEUE_INIT(match_in[i]);
        BT_SPSC_QUEUE_INIT(md_tick[i]);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* BT_QUEUES_H */
