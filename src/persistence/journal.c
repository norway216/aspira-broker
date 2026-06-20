#include "bt_journal.h"
#include "bt_lockfree_queue.h"
#include "bt_timer.h"
#include "bt_cpu.h"
#include "bt_config.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

/* ── Append-Only Journal ──────────────────────────────────────────────
 *
 * Receives journal entries via SPSC ring buffer from hot-path threads.
 * Batches writes and performs periodic fsync for durability.
 * Single writer thread: receives entries, writes to file, fsyncs.
 */

#define JOURNAL_RING_SIZE 262144

BT_SPSC_QUEUE_DEF(bt_journal_ring, bt_journal_entry_t, JOURNAL_RING_SIZE);

struct bt_journal {
    char               path[256];
    int                sync_interval_ms;
    int                fd;
    bt_journal_ring   *ring;
    pthread_t          thread;
    atomic_int          running;
    _Atomic uint64_t   written;
    _Atomic uint64_t   dropped;
};

/* ── Journal worker thread ─────────────────────────────────────────── */
static void *bt_journal_thread_core(void *arg)
{
    bt_journal_t *j = (bt_journal_t *)arg;
    bt_cpu_set_realtime(40);

    uint8_t *write_buf = (uint8_t *)malloc(BT_CFG_JOURNAL_BUF_SIZE);
    if (!write_buf) {
        fprintf(stderr, "[journal] failed to allocate write buffer\n");
        return NULL;
    }

    size_t buf_offset = 0;
    uint64_t last_sync = bt_timer_now_ns();
    uint64_t sync_ns   = (uint64_t)j->sync_interval_ms * 1000000UL;

    fprintf(stderr, "[journal] started, writing to %s\n", j->path);

    while (atomic_load(&j->running)) {
        bt_journal_entry_t entry;
        if (BT_SPSC_POP(*j->ring, &entry)) {
            /* Serialize entry to write buffer */
            size_t entry_size = sizeof(bt_journal_entry_t);
            if (buf_offset + entry_size > BT_CFG_JOURNAL_BUF_SIZE) {
                /* Flush buffer */
                ssize_t written = write(j->fd, write_buf, buf_offset);
                if (written > 0) atomic_fetch_add(&j->written, 1);
                buf_offset = 0;
            }

            memcpy(write_buf + buf_offset, &entry, entry_size);
            buf_offset += entry_size;
        } else {
            /* No data — check if we should flush */
            if (buf_offset > 0) {
                uint64_t now = bt_timer_now_ns();
                if (now - last_sync >= sync_ns) {
                    ssize_t written = write(j->fd, write_buf, buf_offset);
                    if (written > 0) {
                        if (j->sync_interval_ms > 0) fdatasync(j->fd);
                        atomic_fetch_add(&j->written, 1);
                    }
                    buf_offset = 0;
                    last_sync = now;
                }
            }
            BT_CPU_PAUSE();
        }
    }

    /* Final flush */
    if (buf_offset > 0) {
        write(j->fd, write_buf, buf_offset);
        if (j->sync_interval_ms > 0) fdatasync(j->fd);
        atomic_fetch_add(&j->written, 1);
    }

    free(write_buf);
    fprintf(stderr, "[journal] stopped. written=%lu\n", atomic_load(&j->written));
    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────── */

bt_journal_t *bt_journal_open(const char *path, int sync_interval_ms)
{
    bt_journal_t *j = (bt_journal_t *)calloc(1, sizeof(bt_journal_t));
    if (!j) return NULL;

    strncpy(j->path, path, sizeof(j->path) - 1);
    j->sync_interval_ms = sync_interval_ms;

    j->fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_DSYNC, 0644);
    if (j->fd < 0) {
        perror("journal open");
        free(j);
        return NULL;
    }

    j->ring = (bt_journal_ring *)calloc(1, sizeof(bt_journal_ring));
    if (!j->ring) {
        close(j->fd);
        free(j);
        return NULL;
    }
    BT_SPSC_QUEUE_INIT(*j->ring);

    atomic_init(&j->written, 0);
    atomic_init(&j->dropped, 0);
    atomic_init(&j->running, 1);

    if (pthread_create(&j->thread, NULL, bt_journal_thread_core, j) != 0) {
        close(j->fd);
        free(j->ring);
        free(j);
        return NULL;
    }

    return j;
}

int bt_journal_append(bt_journal_t *j, const bt_journal_entry_t *entry)
{
    if (!j || !entry) return -1;
    if (!BT_SPSC_PUSH(*j->ring, *entry)) {
        atomic_fetch_add(&j->dropped, 1);
        return -1;
    }
    return 0;
}

void bt_journal_flush(bt_journal_t *j)
{
    if (!j) return;
    if (j->sync_interval_ms > 0) fdatasync(j->fd);
}

void bt_journal_close(bt_journal_t *j)
{
    if (!j) return;
    atomic_store(&j->running, 0);
    pthread_join(j->thread, NULL);
    close(j->fd);
    free(j->ring);
    free(j);
}

void bt_journal_stats(const bt_journal_t *j, uint64_t *written, uint64_t *dropped)
{
    if (!j) return;
    if (written) *written = atomic_load(&j->written);
    if (dropped) *dropped = atomic_load(&j->dropped);
}
