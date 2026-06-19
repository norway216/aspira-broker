#ifndef BT_TIMER_H
#define BT_TIMER_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── High-Precision Timer ──────────────────────────────────────────── */

/* Get current timestamp in nanoseconds */
static inline uint64_t bt_timer_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000UL + (uint64_t)ts.tv_nsec;
}

/* Get current timestamp in microseconds */
static inline uint64_t bt_timer_now_us(void)
{
    return bt_timer_now_ns() / 1000;
}

/* Read TSC (Time Stamp Counter) — lowest overhead, for hot-path timing */
static inline uint64_t bt_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Read TSC with ordering (prevents reordering) */
static inline uint64_t bt_rdtscp(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    return ((uint64_t)hi << 32) | lo;
}

/* Read TSC with full fence */
static inline uint64_t bt_rdtsc_fence(void)
{
    uint32_t lo, hi;
    __asm__ volatile("mfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Calibrate TSC to nanoseconds (call once at startup) */
double   bt_timer_ticks_per_ns(void);
uint64_t bt_timer_ticks_to_ns(uint64_t ticks);

/* Latency statistics (simple accumulator) */
typedef struct {
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t sum_ns;
    uint64_t count;
    uint64_t histogram[16];   /* powers of 2, idx 0=0-1μs, 1=1-2μs, etc */
} bt_latency_stats_t;

void bt_latency_stats_init(bt_latency_stats_t *s);
void bt_latency_stats_record(bt_latency_stats_t *s, uint64_t latency_ns);
void bt_latency_stats_print(const bt_latency_stats_t *s, const char *label);

/* Percentile computation from sorted array of latencies */
void bt_latency_percentiles(const uint64_t *sorted_latencies, size_t count,
                             double *p50, double *p95, double *p99, double *p999);

#ifdef __cplusplus
}
#endif

#endif /* BT_TIMER_H */
