#include "bt_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Timer Implementation ───────────────────────────────────────────── */

static double g_ticks_per_ns = 0.0;

double bt_timer_ticks_per_ns(void)
{
    if (g_ticks_per_ns > 0.0) return g_ticks_per_ns;

    /* Calibrate: measure TSC over a known time interval */
    const uint64_t calib_ns = 100000000UL; /* 100ms */
    struct timespec ts_start, ts_end;
    uint64_t tsc_start, tsc_end;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start);
    tsc_start = bt_rdtsc_fence();

    /* Busy-wait for ~100ms */
    do {
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts_end);
    } while ((uint64_t)(ts_end.tv_sec - ts_start.tv_sec) * 1000000000UL +
             (uint64_t)ts_end.tv_nsec - (uint64_t)ts_start.tv_nsec < calib_ns);

    tsc_end = bt_rdtsc_fence();

    uint64_t elapsed_ns =
        (uint64_t)(ts_end.tv_sec - ts_start.tv_sec) * 1000000000UL +
        (uint64_t)ts_end.tv_nsec - (uint64_t)ts_start.tv_nsec;

    g_ticks_per_ns = (double)(tsc_end - tsc_start) / (double)elapsed_ns;
    return g_ticks_per_ns;
}

uint64_t bt_timer_ticks_to_ns(uint64_t ticks)
{
    if (g_ticks_per_ns <= 0.0) bt_timer_ticks_per_ns();
    return (uint64_t)((double)ticks / g_ticks_per_ns);
}

/* ── Latency Statistics ─────────────────────────────────────────────── */

void bt_latency_stats_init(bt_latency_stats_t *s)
{
    memset(s, 0, sizeof(*s));
    s->min_ns = UINT64_MAX;
}

void bt_latency_stats_record(bt_latency_stats_t *s, uint64_t latency_ns)
{
    if (latency_ns < s->min_ns) s->min_ns = latency_ns;
    if (latency_ns > s->max_ns) s->max_ns = latency_ns;
    s->sum_ns += latency_ns;
    s->count++;

    /* Histogram bin: powers of 2 in μs (0-1μs, 1-2μs, 2-4μs, 4-8μs...) */
    uint64_t us = latency_ns / 1000;
    int bin = 0;
    if (us > 0) {
        /* find highest set bit */
        while (us >>= 1) bin++;
    }
    if (bin < 16) s->histogram[bin]++;
}

void bt_latency_stats_print(const bt_latency_stats_t *s, const char *label)
{
    if (s->count == 0) {
        printf("[%s] No samples\n", label);
        return;
    }
    double avg = (double)s->sum_ns / (double)s->count;
    printf("[%s] samples=%lu min=%lu ns avg=%.0f ns max=%lu ns\n",
           label, s->count, s->min_ns, avg, s->max_ns);
    printf("  Histogram (μs): ");
    const char *bins[] = {"0-1","1-2","2-4","4-8","8-16","16-32","32-64",
                          "64-128","128-256","256-512","512-1k","1k-2k",
                          "2k-4k","4k-8k","8k-16k","16k+"};
    for (int i = 0; i < 16; i++) {
        if (s->histogram[i] > 0) {
            printf("%s:%lu ", bins[i], s->histogram[i]);
        }
    }
    printf("\n");
}

void bt_latency_percentiles(const uint64_t *sorted_latencies, size_t count,
                             double *p50, double *p95, double *p99, double *p999)
{
    if (count == 0) {
        if (p50)  *p50  = 0;
        if (p95)  *p95  = 0;
        if (p99)  *p99  = 0;
        if (p999) *p999 = 0;
        return;
    }

    #define percentile(arr, n, p) arr[(size_t)((double)(n) * (p))]
    if (p50)  *p50  = (double)percentile(sorted_latencies, count, 0.50);
    if (p95)  *p95  = (double)percentile(sorted_latencies, count, 0.95);
    if (p99)  *p99  = (double)percentile(sorted_latencies, count, 0.99);
    if (p999) *p999 = (double)percentile(sorted_latencies, count, 0.999);
    #undef percentile
}
