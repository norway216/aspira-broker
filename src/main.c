#include "bt_types.h"
#include "bt_config.h"
#include "bt_queues.h"
#include "bt_sequencer.h"
#include "bt_event.h"
#include "bt_clearing.h"
#include "bt_order_gate.h"
#include "bt_oms.h"
#include "bt_risk.h"
#include "bt_matching.h"
#include "bt_recovery.h"
#include "bt_scheduler.h"
#include "bt_timer.h"
#include "bt_cpu.h"
#include "bt_memory_pool.h"
#include "bt_journal.h"
#include "bt_slab_allocator.h"
#include "bt_lockfree_pool.h"
#include "bt_numa.h"
#include "bt_logger.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

/* ── V5 Pipeline Architecture ────────────────────────────────────────
 *
 *  [Gateway] → [OrderGate] → [OMS] → [Risk×2] → [Seq] → [Match×4]
 *      MPSC        MPSC       MPSC      MPSC      MPSC     MPSC
 *                                                             │
 *                                                   ┌─────────┘
 *                                                   ▼ SPSC
 *                                              [Market Data]
 *                                                   │
 *                                          ┌────────┘
 *                                          ▼
 *                                     [Event Bus]  ← V5: event sourcing
 *                                          │
 *                               ┌──────────┼──────────┐
 *                               ▼          ▼          ▼
 *                          [Clearing] [Journal]  [Audit/Future]
 */

/* ── Forward declarations for non-header modules ──────────────────── */

typedef struct gw_ctx_t    gw_ctx_t;
typedef struct bt_md_ctx_t bt_md_ctx_t;

/* Gateway */
gw_ctx_t *bt_gateway_create(int tid, int cpu, int port, int max_conns,
                              int sched_id,
                              bt_gw_oms_queue_t *out,
                              bt_gw_response_queue_t *response_queue);
int  bt_gateway_start(gw_ctx_t *ctx);
void bt_gateway_stop(gw_ctx_t *ctx);
void bt_gateway_destroy(gw_ctx_t *ctx);

/* Market Data */
bt_md_ctx_t *bt_md_create(int tid, int cpu, bt_md_tick_queue_t *in, int sched_id);
int  bt_md_start(bt_md_ctx_t *ctx);
void bt_md_stop(bt_md_ctx_t *ctx);
void bt_md_destroy(bt_md_ctx_t *ctx);

/* Benchmark */
void bt_benchmark_run(int num_orders, int num_symbols, bt_gw_oms_queue_t *queue);

/* ── Global state ──────────────────────────────────────────────────── */
static atomic_int g_running = 1;  /* _Atomic: correct C11 cross-thread visibility */

static bt_mempool_t        g_mempool;
static bt_journal_t       *g_journal    = NULL;
static bt_event_bus_t     *g_event_bus  = NULL;
static bt_risk_state_t    *g_risk_state = NULL;
static bt_sequencer_t     *g_sequencer  = NULL;
static bt_clearing_t      *g_clearing   = NULL;

static bt_oms_ctx_t       *g_oms      = NULL;
static gw_ctx_t           *g_gateway  = NULL;
static bt_order_gate_t    *g_ordergate = NULL;
static bt_risk_worker_t   *g_risk_workers[BT_CFG_RISK_THREADS];
static bt_matching_ctx_t  *g_matchers[BT_CFG_MATCHING_THREADS];
static bt_md_ctx_t        *g_md = NULL;

/* ── Queues (V5 pipeline) ──────────────────────────────────────────── */
static bt_gw_oms_queue_t    g_gate_out_q;      /* Gateway → Order Gate */
static bt_gw_oms_queue_t    g_gate_oms_q;      /* Order Gate → OMS */
static bt_risk_in_queue_t   g_risk_in_q;       /* OMS → Risk */
static bt_seq_in_queue_t    g_seq_in_q;        /* Risk → Sequencer */
static bt_match_in_queue_t  g_match_in_q[BT_CFG_MATCHING_THREADS]; /* Seq → Match */
static bt_md_tick_queue_t   g_md_q[BT_CFG_MATCHING_THREADS];       /* Match → MD */
static bt_gw_response_queue_t g_response_q;                          /* Match → Gateway */

static void *g_seq_out_queues[BT_CFG_MATCHING_THREADS];

static bt_runtime_config_t g_cfg;

/* ── Signal handler ────────────────────────────────────────────────── */
static void sig_handler(int sig) { (void)sig; atomic_store(&g_running, 0); }

/* ── Print banner ──────────────────────────────────────────────────── */
static void print_banner(void)
{
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║   Exchange-Grade Trading System V5                     ║\n");
    printf("║   GW→Gate→OMS→Risk→Seq→Match→MD→EventBus→Clearing    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("  CPU cores: %d  NUMA nodes: %d\n", bt_cpu_count(), bt_numa_num_nodes());
    printf("  Memory pool: %zu MB\n", g_cfg.mempool_size_mb);
    printf("  Matching: %d  Risk: %d  IO: %d\n",
           g_cfg.matching_threads, g_cfg.risk_threads, g_cfg.io_threads);
    printf("  Gateway port: %d\n\n", g_cfg.gateway_port);
}

/* ── Main ──────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    bt_config_default(&g_cfg);

    /* ── CLI argument parsing ────────────────────────────────────────── */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc)
            g_cfg.benchmark_orders = atoi(argv[++i]);
        else if (strcmp(argv[i], "--symbols") == 0 && i + 1 < argc)
            g_cfg.benchmark_symbols = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-bench") == 0)
            g_cfg.run_benchmark = 0;
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            g_cfg.gateway_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--matching-threads") == 0 && i + 1 < argc)
            g_cfg.matching_threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--isolated") == 0)
            g_cfg.use_isolated = 1;
    }

    /* ── Validate configuration ─────────────────────────────────────── */
    if (g_cfg.matching_threads < 1 || g_cfg.matching_threads > 8) {
        fprintf(stderr, "FATAL: matching_threads must be 1-8\n"); return 1;
    }
    if (g_cfg.risk_threads < 1 || g_cfg.risk_threads > 8) {
        fprintf(stderr, "FATAL: risk_threads must be 1-8\n"); return 1;
    }
    if ((g_cfg.matching_threads & (g_cfg.matching_threads - 1)) != 0) {
        fprintf(stderr, "FATAL: matching_threads must be a power of 2\n"); return 1;
    }

    /* Generate core assignments algorithmically */
    for (int i = 0; i < g_cfg.matching_threads; i++)
        g_cfg.cpu_match_cores[i] = g_cfg.cpu_start_core + 6 + i;
    for (int i = 0; i < g_cfg.risk_threads; i++)
        g_cfg.cpu_risk_cores[i] = g_cfg.cpu_start_core + 4 + i;
    g_cfg.cpu_io_cores[0] = g_cfg.cpu_start_core + 2;
    g_cfg.cpu_io_cores[1] = g_cfg.cpu_start_core + 3;

    /* ── V9 Scheduler Init ─────────────────────────────────────────── */
    bt_sched_init(&g_sched);

    /* Register all threads with class, core, priority, budget */
    int sched_gw  = bt_sched_register(&g_sched, "gateway",
        BT_THREAD_CLASS_WARM, g_cfg.cpu_io_cores[0], 60, 200000);
    int sched_og  = bt_sched_register(&g_sched, "ordergate",
        BT_THREAD_CLASS_WARM, g_cfg.cpu_io_cores[1], 55, 100000);
    int sched_oms = bt_sched_register(&g_sched, "oms",
        BT_THREAD_CLASS_WARM, g_cfg.cpu_risk_cores[0] - 1, 70, 200000);
    int sched_seq = bt_sched_register(&g_sched, "sequencer",
        BT_THREAD_CLASS_HOT,  11, 85, 5000);
    int sched_md  = bt_sched_register(&g_sched, "marketdata",
        BT_THREAD_CLASS_WARM, g_cfg.cpu_match_cores[3] + 1, 50, 100000);
    int sched_clr = bt_sched_register(&g_sched, "clearing",
        BT_THREAD_CLASS_COLD, 13, 30, 0);

    int sched_risk[8], sched_match[8];
    for (int i = 0; i < g_cfg.risk_threads; i++) {
        char name[32]; snprintf(name, sizeof(name), "risk-%d", i);
        sched_risk[i] = bt_sched_register(&g_sched, name,
            BT_THREAD_CLASS_WARM, g_cfg.cpu_risk_cores[i], 80, 50000);
    }
    for (int i = 0; i < g_cfg.matching_threads; i++) {
        char name[32]; snprintf(name, sizeof(name), "match-%d", i);
        sched_match[i] = bt_sched_register(&g_sched, name,
            BT_THREAD_CLASS_HOT, g_cfg.cpu_match_cores[i], 90, 10000);
    }

    /* Validate: check collisions, NUMA mixing */
    bt_sched_validate(&g_sched);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    bt_log_init();
    bt_timer_ticks_per_ns();
    print_banner();
    bt_sched_print_map(&g_sched);

    /* ── Memory pool ────────────────────────────────────────────────── */
    size_t pool_sz = g_cfg.mempool_size_mb * 1024UL * 1024UL;
    int arenas = g_cfg.matching_threads + g_cfg.risk_threads + 4;
    if (bt_mempool_init(&g_mempool, pool_sz, arenas, 1) < 0) {
        fprintf(stderr, "FATAL: memory pool init failed\n"); return 1;
    }
    BT_LOG_INFO("Memory pool: %zu MB (NUMA-aware)", g_cfg.mempool_size_mb);

    /* ── Journal ────────────────────────────────────────────────────── */
    g_journal = bt_journal_open(g_cfg.journal_path, g_cfg.journal_sync_ms);
    if (!g_journal) {
        fprintf(stderr, "FATAL: journal open failed\n"); return 1;
    }

    /* ── Crash Recovery (Round 4) ───────────────────────────────────── */
    {
        uint64_t recov_seq = 0, recov_orders = 0, recov_trades = 0;
        int rc = bt_recovery_replay(g_cfg.journal_path,
                                     &recov_seq, &recov_orders, &recov_trades);
        if (rc == 0) {
            BT_LOG_INFO("Recovery: seq=%lu orders=%lu trades=%lu",
                         recov_seq, recov_orders, recov_trades);
        }
    }

    /* ── V5 Event Bus ───────────────────────────────────────────────── */
    g_event_bus = bt_event_bus_create(65536);
    if (!g_event_bus) {
        fprintf(stderr, "FATAL: event bus init failed\n"); return 1;
    }

    /* ── Initialize V5 queues ───────────────────────────────────────── */
    BT_MPSC_QUEUE_INIT(g_gate_out_q);
    BT_MPSC_QUEUE_INIT(g_gate_oms_q);
    BT_MPSC_QUEUE_INIT(g_risk_in_q);
    BT_MPSC_QUEUE_INIT(g_seq_in_q);
    BT_MPSC_QUEUE_INIT(g_response_q);
    for (int i = 0; i < g_cfg.matching_threads; i++) {
        BT_MPSC_QUEUE_INIT(g_match_in_q[i]);
        BT_SPSC_QUEUE_INIT(g_md_q[i]);
    }

    /* ── Risk state ─────────────────────────────────────────────────── */
    g_risk_state = bt_risk_state_create();
    if (!g_risk_state) {
        fprintf(stderr, "FATAL: risk state init failed\n"); return 1;
    }

    /* ── Start subsystems (consumers first, downstream → upstream) ──── */

    /* 1. Clearing (V5: NEW — subscribes to event bus) */
    g_clearing = bt_clearing_create(0, 13, sched_clr);
    if (g_clearing) bt_clearing_start(g_clearing, g_event_bus);

    /* 2. Market Data */
    g_md = bt_md_create(0, g_cfg.cpu_match_cores[3] + 1, &g_md_q[0], sched_md);
    if (g_md) bt_md_start(g_md);

    /* 3. Matching Engines (V5: + event_bus) */
    for (int i = 0; i < g_cfg.matching_threads; i++) {
        bt_mempool_arena_t *arena = bt_mempool_assign_arena(&g_mempool);
        g_matchers[i] = bt_matching_create(i, g_cfg.cpu_match_cores[i],
                                            &g_match_in_q[i], &g_md_q[i],
                                            arena, g_journal, g_event_bus,
                                            &g_response_q);
        if (g_matchers[i]) { bt_matching_start(g_matchers[i]); }
    }

    /* 4. Sequencer */
    g_sequencer = bt_sequencer_create(0, 11, sched_seq);
    if (!g_sequencer) {
        fprintf(stderr, "FATAL: sequencer init failed\n"); return 1;
    }
    {
        for (int i = 0; i < g_cfg.matching_threads; i++)
            g_seq_out_queues[i] = &g_match_in_q[i];
        
        bt_sequencer_start(g_sequencer, &g_seq_in_q, g_seq_out_queues,
                           g_cfg.matching_threads);
    }

    /* 5. Risk Workers */
    for (int i = 0; i < g_cfg.risk_threads; i++) {
        g_risk_workers[i] = bt_risk_worker_create(i, g_cfg.cpu_risk_cores[i],
                                                   &g_risk_in_q, &g_seq_in_q,
                                                   g_cfg.matching_threads,
                                                   g_risk_state);
        if (g_risk_workers[i]) { bt_risk_worker_start(g_risk_workers[i]); }
    }

    /* 6. OMS */
    g_oms = bt_oms_create(0, g_cfg.cpu_risk_cores[0] - 1, &g_gate_oms_q, &g_risk_in_q, sched_oms);
    if (g_oms) bt_oms_start(g_oms);

    /* 7. Order Gate (V5: NEW — between Gateway and OMS) */
    g_ordergate = bt_order_gate_create(0, 2, sched_og);
    if (g_ordergate) {
        
        bt_order_gate_start(g_ordergate, &g_gate_out_q, &g_gate_oms_q,
                            BT_CFG_OMS_QUEUE_CAP);
    }

    /* 8. Gateway (V5: outputs to Order Gate, not directly to OMS) */
    g_gateway = bt_gateway_create(0, g_cfg.cpu_io_cores[0], g_cfg.gateway_port,
                                   g_cfg.max_connections, sched_gw,
                                   &g_gate_out_q, &g_response_q);
    if (g_gateway) bt_gateway_start(g_gateway);

    BT_LOG_INFO("V5 pipeline: GW→Gate→OMS→Risk→Seq→Match×%d→MD→EventBus→Clearing",
                 g_cfg.matching_threads);

    /* ── Benchmark (publishes directly to Order Gate input) ─────────── */
    if (g_cfg.run_benchmark) {
        usleep(100000);
        bt_benchmark_run(g_cfg.benchmark_orders, g_cfg.benchmark_symbols,
                          &g_gate_out_q);
    }

    /* ── Main wait loop ─────────────────────────────────────────────── */
    uint64_t last_health = bt_timer_now_ns();
    while (atomic_load(&g_running)) {
        usleep(1000000);
        if (!atomic_load(&g_running)) break;

        uint64_t now = bt_timer_now_ns();
        if (now - last_health > 5000000000UL) {
            uint64_t jw = 0, jd = 0, seq_orders = 0, seq_global = 0;
            uint64_t ev_pub = 0, ev_del = 0;
            uint64_t clr_trades = 0, clr_ledger = 0;
            double   clr_notional = 0;
            uint64_t gate_recv = 0, gate_pass = 0, gate_rej = 0, gate_thr = 0;

            if (g_journal)   bt_journal_stats(g_journal, &jw, &jd);
            if (g_sequencer) bt_sequencer_stats(g_sequencer, &seq_orders, &seq_global);
            if (g_event_bus) bt_event_bus_stats(g_event_bus, &ev_pub, &ev_del);
            if (g_clearing)  bt_clearing_stats(g_clearing, &clr_trades, &clr_notional, &clr_ledger);
            if (g_ordergate) bt_order_gate_stats(g_ordergate, &gate_recv,
                                                  &gate_pass, &gate_rej, &gate_thr);

            BT_LOG_INFO("V5: seq=%lu/%lu ev=%lu/%lu clr=%lu/%.0f ledger=%lu jw=%lu",
                         seq_orders, seq_global, ev_pub, ev_del,
                         clr_trades, clr_notional, clr_ledger, jw);
            BT_LOG_INFO("V5 Gate: recv=%lu pass=%lu rej=%lu thr=%lu",
                         gate_recv, gate_pass, gate_rej, gate_thr);
            bt_log_flush();
            bt_sched_print_latency(&g_sched);
            last_health = now;
        }
    }

    /* ── Graceful shutdown (upstream first) ─────────────────────────── */
    BT_LOG_INFO("Shutting down..."); bt_log_flush();

    if (g_gateway)   bt_gateway_stop(g_gateway);
    if (g_ordergate) bt_order_gate_stop(g_ordergate);
    if (g_oms)       bt_oms_stop(g_oms);
    for (int i = 0; i < g_cfg.risk_threads; i++)
        if (g_risk_workers[i]) bt_risk_worker_stop(g_risk_workers[i]);
    if (g_sequencer) bt_sequencer_stop(g_sequencer);
    for (int i = 0; i < g_cfg.matching_threads; i++)
        if (g_matchers[i]) bt_matching_stop(g_matchers[i]);
    if (g_md)        bt_md_stop(g_md);
    if (g_clearing)  bt_clearing_stop(g_clearing);

    if (g_journal) { bt_journal_flush(g_journal); bt_journal_close(g_journal); }

    if (g_gateway)   bt_gateway_destroy(g_gateway);
    if (g_ordergate) bt_order_gate_destroy(g_ordergate);
    if (g_oms)       bt_oms_destroy(g_oms);
    for (int i = 0; i < g_cfg.risk_threads; i++)
        if (g_risk_workers[i]) bt_risk_worker_destroy(g_risk_workers[i]);
    if (g_sequencer) bt_sequencer_destroy(g_sequencer);
    for (int i = 0; i < g_cfg.matching_threads; i++)
        if (g_matchers[i]) bt_matching_destroy(g_matchers[i]);
    if (g_md)        bt_md_destroy(g_md);
    if (g_clearing)  bt_clearing_destroy(g_clearing);

    if (g_event_bus) bt_event_bus_destroy(g_event_bus);
    if (g_risk_state) bt_risk_state_destroy(g_risk_state);
    bt_mempool_destroy(&g_mempool);
    bt_log_flush();

    printf("V5 Shutdown complete.\n");
    return 0;
}
