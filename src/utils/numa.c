#define _GNU_SOURCE
#include "bt_numa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sched.h>

#ifdef __linux__
#include <numa.h>
#include <numaif.h>
#endif

int bt_numa_num_nodes(void)
{
#ifdef __linux__
    if (numa_available() < 0) return 1;
    return numa_num_configured_nodes();
#else
    return 1;
#endif
}

int bt_numa_current_node(void)
{
    int cpu = sched_getcpu();
    return bt_numa_node_of_cpu(cpu);
}

int bt_numa_node_of_cpu(int cpu)
{
#ifdef __linux__
    if (numa_available() < 0) return 0;
    return numa_node_of_cpu(cpu);
#else
    (void)cpu;
    return 0;
#endif
}

void *bt_numa_alloc(size_t size, int node)
{
    size_t aligned = (size + 4095) & ~(size_t)4095;

#ifdef __linux__
    if (numa_available() >= 0) {
        void *ptr = numa_alloc_onnode(aligned, node);
        if (ptr) {
            /* Prefault */
            volatile char *p = (volatile char *)ptr;
            for (size_t i = 0; i < aligned; i += 4096) p[i] = 0;
            return ptr;
        }
    }
#else
    (void)node;
#endif

    /* Fallback */
    void *ptr = mmap(NULL, aligned, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return NULL;

    volatile char *p = (volatile char *)ptr;
    for (size_t i = 0; i < aligned; i += 4096) p[i] = 0;
    return ptr;
}

void *bt_numa_alloc_local(size_t size)
{
    return bt_numa_alloc(size, bt_numa_current_node());
}

void bt_numa_free(void *ptr, size_t size)
{
    if (!ptr) return;
    size_t aligned = (size + 4095) & ~(size_t)4095;

#ifdef __linux__
    if (numa_available() >= 0) {
        numa_free(ptr, aligned);
        return;
    }
#endif
    munmap(ptr, aligned);
}

int bt_numa_node_of_ptr(void *ptr)
{
#ifdef __linux__
    if (numa_available() < 0) return -1;
    int node = -1;
    /* Use move_pages to determine the NUMA node of a pointer */
    int status[1];
    void *p = ptr;
    if (move_pages(0, 1, &p, NULL, status, 0) == 0) {
        node = status[0];
    }
    /* Fallback: get_mempolicy */
    if (node < 0) {
        int mode;
        unsigned long nodemask;
        if (get_mempolicy(&mode, &nodemask, sizeof(nodemask) * 8, ptr,
                          MPOL_F_NODE | MPOL_F_ADDR) == 0) {
            node = mode;
        }
    }
    return node;
#else
    (void)ptr;
    return -1;
#endif
}

int bt_numa_bind_thread(int node)
{
#ifdef __linux__
    if (numa_available() < 0) return -1;
    struct bitmask *bm = numa_allocate_nodemask();
    if (!bm) return -1;
    numa_bitmask_setbit(bm, node);
    numa_bind(bm);
    numa_free_nodemask(bm);
    return 0;
#else
    (void)node;
    return -1;
#endif
}

int bt_numa_node_stats(int node, bt_numa_stats_t *stats)
{
    if (!stats) return -1;
    memset(stats, 0, sizeof(*stats));

#ifdef __linux__
    if (numa_available() < 0) return -1;
    long free_bytes = 0;
    stats->total_bytes = (uint64_t)numa_node_size(node, &free_bytes);
    stats->free_bytes  = (uint64_t)free_bytes;
    return 0;
#else
    (void)node;
    return -1;
#endif
}
