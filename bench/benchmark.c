#include "bt_types.h"
#include "bt_config.h"
#include "bt_queues.h"
#include "bt_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Benchmark Harness ────────────────────────────────────────────────
 * Generates synthetic order load and pushes to the OMS input queue.
 */

static uint64_t xorshift64_state = 123456789;

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}
static inline uint64_t xorshift64(void) {
    uint64_t x = xorshift64_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    xorshift64_state = x; return x;
}

void bt_benchmark_run(int num_orders, int num_symbols, bt_gw_oms_queue_t *queue)
{
    if (!queue || num_orders <= 0) return;

    /* Generate symbols (dynamically allocate for variable count) */
    int sym_count = num_symbols < 1 ? 1 : (num_symbols > 256 ? 256 : num_symbols);
    char (*symbols)[16] = (char(*)[16])malloc(sym_count * 16);
    double *base_prices = (double *)malloc(sym_count * sizeof(double));
    if (!symbols || !base_prices) {
        free(symbols); free(base_prices);
        fprintf(stderr, "[bench] malloc failed\n"); return;
    }
    for (int i = 0; i < sym_count; i++) {
        snprintf(symbols[i], 16, "SYM%04d", i);
        base_prices[i] = 100.0 + (double)(xorshift64() % 90000) / 100.0;
    }

    /* Latency tracking */
    uint64_t *latencies = (uint64_t *)malloc(num_orders * sizeof(uint64_t));
    if (!latencies) { fprintf(stderr, "[bench] malloc failed\n"); return; }
    int latency_count = 0;

    bt_latency_stats_t stats;
    bt_latency_stats_init(&stats);

    printf("\n[bench] %d orders, %d symbols\n", num_orders, sym_count);

    uint64_t bench_start = bt_timer_now_ns();
    uint64_t last_report = bench_start;
    uint64_t last_count  = 0;
    int orders_sent = 0, orders_dropped = 0;

    for (int i = 0; i < num_orders; i++) {
        bt_order_request_t req;
        memset(&req, 0, sizeof(req));

        int sym_idx  = xorshift64() % sym_count;
        req.user_id  = (xorshift64() % 1000) + 1;
        strncpy(req.symbol, symbols[sym_idx], sizeof(req.symbol) - 1);
        req.quantity = (uint32_t)((xorshift64() % 1000) + 1);

        uint64_t r = xorshift64() % 100;
        if (r < 60) {
            req.type = BT_TYPE_LIMIT;
            double delta = (double)((int64_t)(xorshift64() % 201) - 100) / 100.0;
            req.price = base_prices[sym_idx] + delta;
            if (req.price <= 0.0) req.price = base_prices[sym_idx];
            base_prices[sym_idx] = req.price;
        } else if (r < 80) {
            req.type = BT_TYPE_MARKET; req.price = 0.0;
        } else if (r < 90) {
            req.type = BT_TYPE_IOC;
            req.price = base_prices[sym_idx] + (double)((int64_t)(xorshift64() % 51) - 25) / 100.0;
        } else {
            req.type = BT_TYPE_FOK;
            req.price = base_prices[sym_idx] + (double)((int64_t)(xorshift64() % 51) - 25) / 100.0;
        }

        req.side = ((xorshift64() % 2) == 0) ? BT_SIDE_BUY : BT_SIDE_SELL;
        uint64_t send_ts = bt_timer_now_ns();
        req.timestamp = send_ts;

        bt_gw_oms_msg_t msg;
        msg.request = req;
        msg.seq_num = 0;

        if (BT_MPSC_PUSH(*queue, msg)) {
            orders_sent++;
            if (latency_count < num_orders) latencies[latency_count++] = send_ts;
        } else {
            orders_dropped++;
            struct timespec ts = {0, 100};
            nanosleep(&ts, NULL);
        }

        uint64_t now = bt_timer_now_ns();
        if (now - last_report > 1000000000UL) {
            double rate = (double)(orders_sent - last_count) / ((double)(now - last_report) / 1e9);
            printf("[bench] sent=%d dropped=%d rate=%.0f/s\n", orders_sent, orders_dropped, rate);
            last_report = now; last_count = orders_sent;
        }
    }

    uint64_t bench_end = bt_timer_now_ns();
    double elapsed = (double)(bench_end - bench_start) / 1e9;

    printf("\n[bench] Complete.\n");
    printf("[bench] Sent: %d  Dropped: %d\n", orders_sent, orders_dropped);
    printf("[bench] Elapsed: %.3f sec  Throughput: %.0f orders/sec\n",
           elapsed, (double)orders_sent / elapsed);

    if (latency_count > 0) {
        uint64_t *lat_ns = (uint64_t *)malloc(latency_count * sizeof(uint64_t));
        if (lat_ns) {
            for (int i = 0; i < latency_count; i++) {
                /* latencies[i] is in ns, bench_start is in ns */
                uint64_t delta_ns = latencies[i] - bench_start;
                lat_ns[i] = delta_ns;
                bt_latency_stats_record(&stats, lat_ns[i]);
            }

            qsort(lat_ns, latency_count, sizeof(uint64_t), cmp_u64);

            double p50, p95, p99, p999;
            bt_latency_percentiles(lat_ns, latency_count, &p50, &p95, &p99, &p999);

            printf("\n[bench] Injection Latency:\n");
            printf("  p50:   %.0f ns (%.3f μs)\n", p50, p50/1000.0);
            printf("  p95:   %.0f ns (%.3f μs)\n", p95, p95/1000.0);
            printf("  p99:   %.0f ns (%.3f μs)\n", p99, p99/1000.0);
            printf("  p99.9: %.0f ns (%.3f μs)\n", p999, p999/1000.0);

            bt_latency_stats_print(&stats, "Injection timing");
            free(lat_ns);
        }
    }
    free(latencies);
    free(symbols);
    free(base_prices);
}
