#define _GNU_SOURCE
#include "bt_cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/* ── CPU Affinity & Thread Management ───────────────────────────────── */

int bt_cpu_pin_thread(int cpu)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

int bt_cpu_pin_pthread(pthread_t thread, int cpu)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    return pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
}

int bt_cpu_set_realtime(int priority)
{
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = priority;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
}

int bt_cpu_mlockall(void)
{
    return mlockall(MCL_CURRENT | MCL_FUTURE);
}

void *bt_cpu_alloc_hugepages(size_t size)
{
    size_t aligned_size = (size + (2UL * 1024 * 1024) - 1) & ~(2UL * 1024 * 1024 - 1);

    void *ptr = mmap(NULL, aligned_size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                     -1, 0);

    if (ptr == MAP_FAILED) {
        /* Fallback: try without MAP_HUGETLB */
        ptr = mmap(NULL, aligned_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
        if (ptr == MAP_FAILED) return NULL;

        /* Suggest transparent hugepages */
        madvise(ptr, aligned_size, MADV_HUGEPAGE);
    }

    return ptr;
}

void bt_cpu_free_hugepages(void *ptr, size_t size)
{
    if (!ptr) return;
    size_t aligned_size = (size + (2UL * 1024 * 1024) - 1) & ~(2UL * 1024 * 1024 - 1);
    munmap(ptr, aligned_size);
}

int bt_cpu_count(void)
{
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
}

int bt_cpu_numa_node(int cpu)
{
    /* Simple fallback — proper implementation would parse /sys/devices/system/node */
    (void)cpu;
    return 0;
}

void bt_cpu_madvise_hugepage(void *ptr, size_t size)
{
    if (ptr && size > 0) {
        madvise(ptr, size, MADV_HUGEPAGE);
    }
}

void bt_cpu_prefault(void *ptr, size_t size)
{
    if (!ptr || size == 0) return;

    /* Touch every page to fault it in */
    volatile char *p = (volatile char *)ptr;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < size; i += page_size) {
        p[i] = 0;
    }
}
