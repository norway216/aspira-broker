#define _GNU_SOURCE
#include "bt_hugepage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int bt_hugepage_init(bt_hugepage_t *hp, size_t total_size, int use_1gb)
{
    if (!hp) return -1;
    memset(hp, 0, sizeof(*hp));

    /* Align to 2MB boundary */
    size_t aligned = (total_size + (2UL * 1024 * 1024) - 1) & ~(2UL * 1024 * 1024 - 1);
    hp->total_size = aligned;
    hp->use_1gb = use_1gb;

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    /* Try 1GB pages first if requested */
    if (use_1gb) {
        hp->base = mmap(NULL, aligned, PROT_READ | PROT_WRITE,
                        flags | MAP_HUGETLB | (30 << MAP_HUGE_SHIFT), -1, 0);
        if (hp->base != MAP_FAILED) goto success;
    }

    /* Try 2MB pages */
    hp->base = mmap(NULL, aligned, PROT_READ | PROT_WRITE,
                    flags | MAP_HUGETLB, -1, 0);
    if (hp->base != MAP_FAILED) goto success;

    /* Fallback: regular pages with transparent hugepage hint */
    hp->base = mmap(NULL, aligned, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (hp->base == MAP_FAILED) {
        perror("mmap hugepage");
        return -1;
    }
    madvise(hp->base, aligned, MADV_HUGEPAGE);

success:
    /* Prefault all pages */
    volatile char *p = (volatile char *)hp->base;
    for (size_t i = 0; i < aligned; i += 4096) p[i] = 0;

    hp->offset = 0;
    return 0;
}

void *bt_hugepage_alloc(bt_hugepage_t *hp, size_t size)
{
    if (!hp || !hp->base) return NULL;

    /* Align to 2MB for hugepage-friendly access */
    size_t aligned = (size + (2UL * 1024 * 1024) - 1) & ~(2UL * 1024 * 1024 - 1);

    if (hp->offset + aligned > hp->total_size) return NULL;

    void *ptr = (uint8_t *)hp->base + hp->offset;
    hp->offset += aligned;
    return ptr;
}

void bt_hugepage_reset(bt_hugepage_t *hp)
{
    if (hp) hp->offset = 0;
}

void bt_hugepage_destroy(bt_hugepage_t *hp)
{
    if (!hp || !hp->base) return;
    munmap(hp->base, hp->total_size);
    memset(hp, 0, sizeof(*hp));
}
