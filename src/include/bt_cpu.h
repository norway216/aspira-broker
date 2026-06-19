#ifndef BT_CPU_H
#define BT_CPU_H

#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── CPU Affinity & Thread Management ───────────────────────────────── */

/**
 * Pin the calling thread to a specific CPU core.
 * @param cpu  CPU core index (0-based)
 * @return 0 on success, -1 on error
 */
int bt_cpu_pin_thread(int cpu);

/**
 * Pin a pthread to a specific CPU core.
 * @param thread  pthread handle
 * @param cpu     CPU core index (0-based)
 * @return 0 on success, -1 on error
 */
int bt_cpu_pin_pthread(pthread_t thread, int cpu);

/**
 * Set the calling thread to SCHED_FIFO real-time scheduling.
 * @param priority  Priority level (1-99, higher = more priority)
 * @return 0 on success, -1 on error
 */
int bt_cpu_set_realtime(int priority);

/**
 * Lock all current and future memory pages to prevent paging.
 * @return 0 on success, -1 on error (may need CAP_IPC_LOCK or root)
 */
int bt_cpu_mlockall(void);

/**
 * Allocate memory backed by HugePages (2MB pages).
 * @param size  Size in bytes (will be rounded up to 2MB)
 * @return pointer on success, NULL on failure
 */
void *bt_cpu_alloc_hugepages(size_t size);

/**
 * Free HugePages-backed memory.
 * @param ptr   Pointer returned by bt_cpu_alloc_hugepages
 * @param size  Original allocation size
 */
void bt_cpu_free_hugepages(void *ptr, size_t size);

/**
 * Get the number of available CPU cores.
 */
int bt_cpu_count(void);

/**
 * Get the NUMA node for a given CPU core.
 */
int bt_cpu_numa_node(int cpu);

/**
 * Suggest transparent hugepage usage for a memory region.
 */
void bt_cpu_madvise_hugepage(void *ptr, size_t size);

/**
 * Prefault memory pages to avoid page faults in the hot path.
 */
void bt_cpu_prefault(void *ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* BT_CPU_H */
