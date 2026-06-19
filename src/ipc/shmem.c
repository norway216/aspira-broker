#include "bt_types.h"
#include "bt_config.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

/* ── Shared Memory IPC ────────────────────────────────────────────────
 *
 * Provides shared memory regions for cross-process communication.
 * In the single-process mode, this is not used on the hot path.
 * For multi-process deployment, order requests are passed via
 * lock-free ring buffers in shared memory.
 *
 * Layout of the shared memory segment:
 *   [header: metadata, sequence numbers, atomic flags]
 *   [ring buffer: SPSC queue for orders]
 *   [ring buffer: SPSC queue for trades/responses]
 */

#define SHMEM_MAGIC     0x4254494D  /* "BTIM" */
#define SHMEM_VERSION   1
#define SHMEM_RING_SIZE BT_CFG_GATEWAY_QUEUE_CAP

typedef struct {
    uint32_t    magic;
    uint32_t    version;
    uint64_t    segment_size;
    char        name[64];
    /* SPSC ring for orders (gateway → OMS) */
    _Atomic uint64_t order_head;
    _Atomic uint64_t order_tail;
    uint64_t    order_mask;
    /* SPSC ring for trades (matching → gateway responses) */
    _Atomic uint64_t trade_head;
    _Atomic uint64_t trade_tail;
    uint64_t    trade_mask;
    uint64_t    create_time;
    uint64_t    update_time;
    /* Data follows: order_ring + trade_ring */
    uint8_t     data[];
} bt_shmem_header_t;

typedef struct {
    bt_shmem_header_t *header;
    void              *base;
    size_t             size;
    int                fd;
    int                owner; /* 1 if created, 0 if attached */
} bt_shmem_t;

/* ── Public API ────────────────────────────────────────────────────── */

bt_shmem_t *bt_shmem_create(const char *name, size_t size)
{
    size_t aligned = (size + 4095) & ~(size_t)4095;
    size_t total = aligned + sizeof(bt_shmem_header_t);

    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }

    if (ftruncate(fd, (off_t)total) < 0) {
        perror("ftruncate");
        close(fd);
        shm_unlink(name);
        return NULL;
    }

    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        shm_unlink(name);
        return NULL;
    }

    bt_shmem_t *shm = (bt_shmem_t *)calloc(1, sizeof(bt_shmem_t));
    if (!shm) {
        munmap(base, total);
        close(fd);
        shm_unlink(name);
        return NULL;
    }

    shm->header = (bt_shmem_header_t *)base;
    shm->base   = base;
    shm->size   = total;
    shm->fd     = fd;
    shm->owner  = 1;

    /* Initialize header */
    memset(shm->header, 0, sizeof(bt_shmem_header_t));
    shm->header->magic        = SHMEM_MAGIC;
    shm->header->version      = SHMEM_VERSION;
    shm->header->segment_size = total;
    strncpy(shm->header->name, name, sizeof(shm->header->name) - 1);
    shm->header->order_mask   = SHMEM_RING_SIZE - 1;
    shm->header->trade_mask   = SHMEM_RING_SIZE - 1;
    atomic_init(&shm->header->order_head, 0);
    atomic_init(&shm->header->order_tail, 0);
    atomic_init(&shm->header->trade_head, 0);
    atomic_init(&shm->header->trade_tail, 0);
    shm->header->create_time  = (uint64_t)time(NULL);

    return shm;
}

bt_shmem_t *bt_shmem_attach(const char *name)
{
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        perror("shm_open (attach)");
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return NULL;
    }

    void *base = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return NULL;
    }

    bt_shmem_header_t *hdr = (bt_shmem_header_t *)base;
    if (hdr->magic != SHMEM_MAGIC) {
        fprintf(stderr, "shmem: invalid magic\n");
        munmap(base, (size_t)st.st_size);
        close(fd);
        return NULL;
    }

    bt_shmem_t *shm = (bt_shmem_t *)calloc(1, sizeof(bt_shmem_t));
    if (!shm) {
        munmap(base, (size_t)st.st_size);
        close(fd);
        return NULL;
    }

    shm->header = hdr;
    shm->base   = base;
    shm->size   = (size_t)st.st_size;
    shm->fd     = fd;
    shm->owner  = 0;

    return shm;
}

void bt_shmem_detach(bt_shmem_t *shm)
{
    if (!shm) return;
    munmap(shm->base, shm->size);
    close(shm->fd);
    free(shm);
}

void bt_shmem_destroy(bt_shmem_t *shm)
{
    if (!shm) return;
    char name[64];
    strncpy(name, shm->header->name, sizeof(name) - 1);
    name[63] = '\0';
    munmap(shm->base, shm->size);
    close(shm->fd);
    if (shm->owner) shm_unlink(name);
    free(shm);
}
