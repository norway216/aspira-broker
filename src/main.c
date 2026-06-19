#include "bt_types.h"
#include "bt_config.h"
#include "bt_queues.h"
#include "bt_timer.h"
#include "bt_cpu.h"
#include "bt_memory_pool.h"
#include "bt_journal.h"
#include "bt_logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

/* ── Forward declarations ──────────────────────────────────────────── */

/* OMS */
typedef struct bt_oms_ctx_t bt_oms_ctx_t;
bt_oms_ctx_t *bt_oms_create(int thread_id, int cpu_core,
                             bt_gw_oms_queue_t *in, bt_risk_in_queue_t *out);
int  bt_oms_start(bt_oms_ctx_t *ctx);
void bt_oms_stop(bt_oms_ctx_t *ctx);
void bt_oms_destroy(bt_oms_ctx_t *ctx);

/* Risk Engine */
typedef struct bt_risk_state_t  bt_risk_state_t;
typedef struct bt_risk_worker_t bt_risk_worker_t;
bt_risk_state_t  *bt_risk_state_create(void);
void              bt_risk_state_destroy(bt_risk_state_t *s);
void              bt_risk_kill_switch(bt_risk_state_t *s, int on);
bt_risk_worker_t *bt_risk_worker_create(int wid, int cpu, bt_risk_in_queue_t *in,
                                         bt_match_in_queue_t *out, int nout,
                                         bt_risk_state_t *st);
int  bt_risk_worker_start(bt_risk_worker_t *w);
void bt_risk_worker_stop(bt_risk_worker_t *w);
void bt_risk_worker_destroy(bt_risk_worker_t *w);

/* Matching Engine */
typedef struct bt_matching_ctx_t bt_matching_ctx_t;
bt_matching_ctx_t *bt_matching_create(int tid, int cpu,
                                       bt_match_in_queue_t *in,
                                       bt_md_tick_queue_t *md,
                                       bt_mempool_arena_t *arena,
                                       bt_journal_t *journal);
int  bt_matching_start(bt_matching_ctx_t *ctx);
void bt_matching_stop(bt_matching_ctx_t *ctx);
void bt_matching_destroy(bt_matching_ctx_t *ctx);

/* Gateway */
typedef struct gw_ctx_t gw_ctx_t;
gw_ctx_t *bt_gateway_create(int tid, int cpu, int port, int max_conns,
                              bt_gw_oms_queue_t *out);
int  bt_gateway_start(gw_ctx_t *ctx);
void bt_gateway_stop(gw_ctx_t *ctx);
void bt_gateway_destroy(gw_ctx_t *ctx);

/* Market Data */
typedef struct bt_md_ctx_t bt_md_ctx_t;
bt_md_ctx_t *bt_md_create(int tid, int cpu, bt_md_tick_queue_t *in);
int  bt_md_start(bt_md_ctx_t *ctx);
void bt_md_stop(bt_md_ctx_t *ctx);
void bt_md_destroy(bt_md_ctx_t *ctx);

/* Benchmark */
void bt_benchmark_run(int num_orders, int num_symbols, bt_gw_oms_queue_t *queue);

/* ── Global state ──────────────────────────────────────────────────── */
static volatile int g_running = 1;

static bt_mempool_t        g_mempool;
static bt_journal_t       *g_journal = NULL;
static bt_risk_state_t    *g_risk_state = NULL;

static bt_oms_ctx_t       *g_oms = NULL;
static gw_ctx_t           *g_gateway = NULL;
static bt_risk_worker_t   *g_risk_workers[BT_CFG_RISK_THREADS];
static bt_matching_ctx_t  *g_matchers[BT_CFG_MATCHING_THREADS];
static bt_md_ctx_t        *g_md = NULL;

static bt_gw_oms_queue_t    g_gw_oms_q;
static bt_oms_risk_queue_t  g_oms_risk_q;
static bt_risk_in_queue_t   g_risk_in_q;
static bt_match_in_queue_t  g_match_in_q[BT_CFG_MATCHING_THREADS];
static bt_md_tick_queue_t   g_md_q[BT_CFG_MATCHING_THREADS];

static bt_runtime_config_t g_cfg;

/* ── Signal handler ────────────────────────────────────────────────── */
static void sig_handler(int sig) { (void)sig; g_running = 0; }

/* ── Print banner ──────────────────────────────────────────────────── */
static void print_banner(void)
{
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║   Broker-Grade Trading System v2.0                ║\n");
    printf("║   High-Performance Matching Engine                ║\n");
    printf("╚════════════════════════════════════════════════════╝\n");
    printf("  CPU cores: %d\n", bt_cpu_count());
    printf("  Memory pool: %zu MB\n", g_cfg.mempool_size_mb);
    printf("  Matching threads: %d  Risk threads: %d  IO threads: %d\n",
           g_cfg.matching_threads, g_cfg.risk_threads, g_cfg.io_threads);
    printf("  Gateway port: %d\n\n", g_cfg.gateway_port);
}

/* ── Main ──────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    bt_config_default(&g_cfg);

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
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    bt_log_init();
    bt_timer_ticks_per_ns();
    print_banner();

    /* ── Memory pool ────────────────────────────────────────────────── */
    size_t pool_sz = g_cfg.mempool_size_mb * 1024UL * 1024UL;
    int arenas = g_cfg.matching_threads + g_cfg.risk_threads + 2;
    if (bt_mempool_init(&g_mempool, pool_sz, arenas, 1) < 0) {
        fprintf(stderr, "FATAL: memory pool init failed\n"); return 1;
    }
    BT_LOG_INFO("Memory pool: %zu MB", g_cfg.mempool_size_mb);

    /* ── Journal ────────────────────────────────────────────────────── */
    g_journal = bt_journal_open(g_cfg.journal_path, g_cfg.journal_sync_ms);

    /* ── Initialize queues ──────────────────────────────────────────── */
    bt_queues_init_all(&g_gw_oms_q, &g_oms_risk_q, &g_risk_in_q,
                        g_match_in_q, g_md_q,
                        g_cfg.matching_threads);

    /* ── Risk state ─────────────────────────────────────────────────── */
    g_risk_state = bt_risk_state_create();

    /* ── Start subsystems (consumers first) ─────────────────────────── */

    /* 1. Market Data */
    g_md = bt_md_create(0, g_cfg.cpu_match_cores[3] + 1, &g_md_q[0]);
    if (g_md) bt_md_start(g_md);

    /* 2. Matching Engines */
    for (int i = 0; i < g_cfg.matching_threads; i++) {
        bt_mempool_arena_t *arena = bt_mempool_assign_arena(&g_mempool);
        g_matchers[i] = bt_matching_create(i, g_cfg.cpu_match_cores[i],
                                            &g_match_in_q[i], &g_md_q[i],
                                            arena, g_journal);
        if (g_matchers[i]) bt_matching_start(g_matchers[i]);
    }

    /* 3. Risk Workers */
    for (int i = 0; i < g_cfg.risk_threads; i++) {
        g_risk_workers[i] = bt_risk_worker_create(i, g_cfg.cpu_risk_cores[i],
                                                   &g_risk_in_q, g_match_in_q,
                                                   g_cfg.matching_threads,
                                                   g_risk_state);
        if (g_risk_workers[i]) bt_risk_worker_start(g_risk_workers[i]);
    }

    /* 4. OMS */
    g_oms = bt_oms_create(0, g_cfg.cpu_risk_cores[0] - 1, &g_gw_oms_q, &g_risk_in_q);
    if (g_oms) bt_oms_start(g_oms);

    /* 5. Gateway */
    g_gateway = bt_gateway_create(0, g_cfg.cpu_io_cores[0], g_cfg.gateway_port,
                                   g_cfg.max_connections, &g_gw_oms_q);
    if (g_gateway) bt_gateway_start(g_gateway);

    BT_LOG_INFO("All subsystems started. Running...");

    /* ── Benchmark ──────────────────────────────────────────────────── */
    if (g_cfg.run_benchmark) {
        usleep(100000);
        bt_benchmark_run(g_cfg.benchmark_orders, g_cfg.benchmark_symbols, &g_gw_oms_q);
    }

    /* ── Main wait loop ─────────────────────────────────────────────── */
    uint64_t last_health = bt_timer_now_ns();
    while (g_running) {
        usleep(1000000);
        if (!g_running) break;

        uint64_t now = bt_timer_now_ns();
        if (now - last_health > 5000000000UL) {
            uint64_t jw = 0, jd = 0;
            if (g_journal) bt_journal_stats(g_journal, &jw, &jd);
            BT_LOG_INFO("Health: journal w=%lu d=%lu", jw, jd);
            bt_log_flush();
            last_health = now;
        }
    }

    /* ── Graceful shutdown ──────────────────────────────────────────── */
    BT_LOG_INFO("Shutting down..."); bt_log_flush();

    if (g_gateway)  bt_gateway_stop(g_gateway);
    if (g_oms)      bt_oms_stop(g_oms);
    for (int i = 0; i < g_cfg.risk_threads; i++)
        if (g_risk_workers[i]) bt_risk_worker_stop(g_risk_workers[i]);
    for (int i = 0; i < g_cfg.matching_threads; i++)
        if (g_matchers[i]) bt_matching_stop(g_matchers[i]);
    if (g_md)       bt_md_stop(g_md);

    if (g_journal) { bt_journal_flush(g_journal); bt_journal_close(g_journal); }

    if (g_gateway)  bt_gateway_destroy(g_gateway);
    if (g_oms)      bt_oms_destroy(g_oms);
    for (int i = 0; i < g_cfg.risk_threads; i++)
        if (g_risk_workers[i]) bt_risk_worker_destroy(g_risk_workers[i]);
    for (int i = 0; i < g_cfg.matching_threads; i++)
        if (g_matchers[i]) bt_matching_destroy(g_matchers[i]);
    if (g_md)       bt_md_destroy(g_md);

    if (g_risk_state) bt_risk_state_destroy(g_risk_state);
    bt_mempool_destroy(&g_mempool);
    bt_log_flush();

    printf("Shutdown complete.\n");
    return 0;
}
