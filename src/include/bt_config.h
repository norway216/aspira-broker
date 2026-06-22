#ifndef BT_CONFIG_H
#define BT_CONFIG_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── System Configuration ──────────────────────────────────────────── */

/* Number of matching engine threads (one per symbol shard) */
#define BT_CFG_MATCHING_THREADS    4

/* Number of risk engine worker threads */
#define BT_CFG_RISK_THREADS        2

/* Number of I/O (gateway) threads */
#define BT_CFG_IO_THREADS          2

/* Number of market data threads */
#define BT_CFG_MD_THREADS          1

/* Number of journal writer threads */
#define BT_CFG_JOURNAL_THREADS     1

/* ── Queue Capacities (must be powers of 2 for lock-free mask ops) ──── */
#define BT_CFG_GATEWAY_QUEUE_CAP   65536
#define BT_CFG_OMS_QUEUE_CAP       65536
#define BT_CFG_RISK_QUEUE_CAP      65536
#define BT_CFG_MATCH_QUEUE_CAP     65536
#define BT_CFG_TRADE_QUEUE_CAP    131072
#define BT_CFG_JOURNAL_QUEUE_CAP  262144

/* Compile-time power-of-2 validation for all queue capacities */
#define _BT_IS_POW2(x) (((x) != 0) && (((x) & ((x) - 1)) == 0))
#ifdef __cplusplus
static_assert(_BT_IS_POW2(BT_CFG_GATEWAY_QUEUE_CAP), "GATEWAY_QUEUE_CAP must be power of 2");
static_assert(_BT_IS_POW2(BT_CFG_OMS_QUEUE_CAP),     "OMS_QUEUE_CAP must be power of 2");
static_assert(_BT_IS_POW2(BT_CFG_RISK_QUEUE_CAP),    "RISK_QUEUE_CAP must be power of 2");
static_assert(_BT_IS_POW2(BT_CFG_MATCH_QUEUE_CAP),   "MATCH_QUEUE_CAP must be power of 2");
static_assert(_BT_IS_POW2(BT_CFG_TRADE_QUEUE_CAP),   "TRADE_QUEUE_CAP must be power of 2");
static_assert(_BT_IS_POW2(BT_CFG_JOURNAL_QUEUE_CAP), "JOURNAL_QUEUE_CAP must be power of 2");
#else
_Static_assert(_BT_IS_POW2(BT_CFG_GATEWAY_QUEUE_CAP), "GATEWAY_QUEUE_CAP must be power of 2");
_Static_assert(_BT_IS_POW2(BT_CFG_OMS_QUEUE_CAP),     "OMS_QUEUE_CAP must be power of 2");
_Static_assert(_BT_IS_POW2(BT_CFG_RISK_QUEUE_CAP),    "RISK_QUEUE_CAP must be power of 2");
_Static_assert(_BT_IS_POW2(BT_CFG_MATCH_QUEUE_CAP),   "MATCH_QUEUE_CAP must be power of 2");
_Static_assert(_BT_IS_POW2(BT_CFG_TRADE_QUEUE_CAP),   "TRADE_QUEUE_CAP must be power of 2");
_Static_assert(_BT_IS_POW2(BT_CFG_JOURNAL_QUEUE_CAP), "JOURNAL_QUEUE_CAP must be power of 2");
#endif

/* ── Memory Pool Configuration ──────────────────────────────────────── */
#define BT_CFG_MEMPOOL_SIZE_MB     4096
#define BT_CFG_MAX_ORDERS          (1 << 20)   /* 1M orders */
#define BT_CFG_MAX_TRADES          (1 << 20)   /* 1M trades */

/* ── Network Configuration ──────────────────────────────────────────── */
#define BT_CFG_GATEWAY_PORT        9000
#define BT_CFG_MAX_CONNECTIONS     1024
#define BT_CFG_RECV_BUF_SIZE       (64 * 1024)
#define BT_CFG_SEND_BUF_SIZE       (64 * 1024)
#define BT_CFG_RATE_LIMIT_RPS      10000       /* requests per second */

/* ── Risk Configuration ─────────────────────────────────────────────── */
#define BT_CFG_RISK_MAX_POSITION   10000000    /* max position per symbol */
#define BT_CFG_RISK_MAX_EXPOSURE   50000000.0  /* max total notional */
#define BT_CFG_RISK_MARGIN_RATE    0.20        /* 20% margin */
#define BT_CFG_RISK_CIRCUIT_BREAKER_THRESH 100000 /* orders/sec trigger */

/* ── Journal Configuration ──────────────────────────────────────────── */
#define BT_CFG_JOURNAL_PATH        "/tmp/bt_journal.log"
#define BT_CFG_JOURNAL_SYNC_MS     1           /* fsync interval */
#define BT_CFG_JOURNAL_BUF_SIZE    (16 * 1024 * 1024)  /* 16MB buffer */

/* ── Market Data Configuration ──────────────────────────────────────── */
#define BT_CFG_MD_SNAPSHOT_INTERVAL_MS  100
#define BT_CFG_MD_MAX_SNAPSHOT_LEVELS   10

/* ── Benchmark Configuration ────────────────────────────────────────── */
#define BT_CFG_BENCH_NUM_ORDERS    100000
#define BT_CFG_BENCH_SYMBOLS       10

/* ── Runtime configuration (set from main) ──────────────────────────── */
typedef struct {
    int     matching_threads;
    int     risk_threads;
    int     io_threads;
    int     md_threads;
    int     journal_threads;

    int     gateway_port;
    int     max_connections;
    int     rate_limit_rps;

    size_t  mempool_size_mb;

    int     journal_sync_ms;
    char    journal_path[256];

    int     cpu_start_core;   /* first core to pin threads to */
    int     cpu_io_cores[4];
    int     cpu_risk_cores[8];
    int     cpu_match_cores[8];

    int     log_level;        /* 0=err, 1=warn, 2=info, 3=debug */
    int     run_benchmark;
    int     benchmark_orders;
    int     benchmark_symbols;
    int     use_isolated;      /* use process isolation for matching */
    int     safe_mode;         /* V11: reject new orders, allow cancels */
} bt_runtime_config_t;

/* Default runtime config */
static inline void bt_config_default(bt_runtime_config_t *cfg)
{
    cfg->matching_threads  = BT_CFG_MATCHING_THREADS;
    cfg->risk_threads      = BT_CFG_RISK_THREADS;
    cfg->io_threads        = BT_CFG_IO_THREADS;
    cfg->md_threads        = BT_CFG_MD_THREADS;
    cfg->journal_threads   = BT_CFG_JOURNAL_THREADS;
    cfg->gateway_port      = BT_CFG_GATEWAY_PORT;
    cfg->max_connections   = BT_CFG_MAX_CONNECTIONS;
    cfg->rate_limit_rps    = BT_CFG_RATE_LIMIT_RPS;
    cfg->mempool_size_mb   = BT_CFG_MEMPOOL_SIZE_MB;
    cfg->journal_sync_ms   = BT_CFG_JOURNAL_SYNC_MS;
    cfg->cpu_start_core    = 0;
    cfg->log_level         = 2;
    cfg->run_benchmark     = 1;
    cfg->benchmark_orders  = BT_CFG_BENCH_NUM_ORDERS;
    cfg->benchmark_symbols = BT_CFG_BENCH_SYMBOLS;

    /* default CPU layout: leave cores 0-1 free for OS */
    cfg->cpu_io_cores[0]    = 2;
    cfg->cpu_io_cores[1]    = 3;
    cfg->cpu_risk_cores[0]  = 4;
    cfg->cpu_risk_cores[1]  = 5;
    cfg->cpu_match_cores[0] = 6;
    cfg->cpu_match_cores[1] = 7;
    cfg->cpu_match_cores[2] = 8;
    cfg->cpu_match_cores[3] = 9;

    /* journal path */
    snprintf(cfg->journal_path, sizeof(cfg->journal_path),
             "%s", BT_CFG_JOURNAL_PATH);
}

#ifdef __cplusplus
}
#endif

#endif /* BT_CONFIG_H */
