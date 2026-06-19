#ifndef BT_NUMA_H
#define BT_NUMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── NUMA-Aware Memory Allocation ────────────────────────────────────
 * Provides memory allocation from a specific NUMA node.
 * Each NUMA node has its own memory pool for local access.
 */

/**
 * Get the number of NUMA nodes available on the system.
 */
int bt_numa_num_nodes(void);

/**
 * Get the NUMA node for the calling thread's current CPU.
 */
int bt_numa_current_node(void);

/**
 * Get the NUMA node for a given CPU core.
 */
int bt_numa_node_of_cpu(int cpu);

/**
 * Allocate memory on a specific NUMA node.
 * Uses mbind/mmap to ensure local allocation.
 * Falls back to regular mmap if NUMA is unavailable.
 */
void *bt_numa_alloc(size_t size, int node);

/**
 * Allocate memory on the local NUMA node (for the calling thread).
 */
void *bt_numa_alloc_local(size_t size);

/**
 * Free memory allocated with bt_numa_alloc.
 */
void bt_numa_free(void *ptr, size_t size);

/**
 * Get the NUMA node for an existing memory allocation.
 * Returns -1 if unknown.
 */
int bt_numa_node_of_ptr(void *ptr);

/**
 * Bind the calling thread to a specific NUMA node.
 * (Memory allocations from this thread will prefer the bound node.)
 */
int bt_numa_bind_thread(int node);

/**
 * Get NUMA node memory statistics.
 */
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t alloc_bytes;
} bt_numa_stats_t;

int bt_numa_node_stats(int node, bt_numa_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* BT_NUMA_H */
