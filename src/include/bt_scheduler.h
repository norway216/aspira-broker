/* ── V9 Deterministic Thread Scheduler ────────────────────────────────
 * Thread class system, CPU core allocation, NUMA binding,
 * latency budget enforcement, and scheduling telemetry.
 *
 * V9 Principle: "Not only isolate the system, but also precisely
 * control how it executes in time and space." */

#ifndef BT_SCHEDULER_H
#define BT_SCHEDULER_H

#include "bt_types.h"
#include "bt_timer.h"
#include <pthread.h>
#include <stdio.h>

#define BT_SCHED_MAX_THREADS    32

/* ── Thread class ──────────────────────────────────────────────────── */
typedef enum {
    BT_THREAD_CLASS_HOT  = 0,  /* matching, sequencer — never preempted */
    BT_THREAD_CLASS_WARM = 1,  /* risk, OMS, gateway — bounded latency */
    BT_THREAD_CLASS_COLD = 2   /* clearing, journal — background */
} bt_thread_class_t;

/* ── Scheduler entry (one per thread) ──────────────────────────────── */
typedef struct {
    char              name[32];
    bt_thread_class_t class;
    int               cpu;             /* assigned core */
    int               numa_node;       /* derived from cpu */
    int               priority;        /* SCHED_FIFO 1-99 */
    uint64_t          budget_ns;       /* latency budget (0 = none) */
    bt_latency_stats_t stats;          /* accumulated latency */
    int               registered;      /* 1 = active */
} bt_sched_entry_t;

/* ── Scheduler state ───────────────────────────────────────────────── */
typedef struct {
    bt_sched_entry_t  entries[BT_SCHED_MAX_THREADS];
    int               count;
    int               cpu_map[256];    /* cpu -> entry index, -1 = free */
    int               total_cpus;
    int               validated;
} bt_scheduler_t;

/* ── Global scheduler instance ─────────────────────────────────────── */
extern bt_scheduler_t g_sched;

/* ── API ───────────────────────────────────────────────────────────── */

/* Initialize scheduler: detect topology, clear entries */
void bt_sched_init(bt_scheduler_t *s);

/* Register a thread with class, core, priority, and latency budget.
 * Returns entry index (handle) or -1 on error. */
int  bt_sched_register(bt_scheduler_t *s, const char *name,
                        bt_thread_class_t cls, int cpu, int priority,
                        uint64_t budget_ns);

/* Apply scheduling: pin calling thread to its assigned core,
 * set SCHED_FIFO priority, bind to NUMA node. Call from thread fn. */
void bt_sched_apply(bt_scheduler_t *s, int entry_id);

/* Validate: check for core collisions, class mixing on same NUMA node.
 * Returns 0 if valid, -1 with error printed if conflicts found. */
int  bt_sched_validate(bt_scheduler_t *s);

/* Record a latency sample for the thread. Checks budget if set. */
void bt_sched_record_latency(bt_scheduler_t *s, int entry_id,
                              uint64_t lat_ns);

/* Print the core allocation map (called at startup). */
void bt_sched_print_map(bt_scheduler_t *s);

/* Print per-thread latency summaries (called at health check). */
void bt_sched_print_latency(bt_scheduler_t *s);

/* Class-to-string helper */
const char *bt_sched_class_name(bt_thread_class_t cls);

#endif /* BT_SCHEDULER_H */
