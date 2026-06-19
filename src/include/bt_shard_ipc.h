#ifndef BT_SHARD_IPC_H
#define BT_SHARD_IPC_H

#include "bt_types.h"
#include "bt_queues.h"
#include "bt_memory_pool.h"
#include "bt_journal.h"
#include "bt_event.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── V6 Shard IPC ────────────────────────────────────────────────────
 * Process isolation for matching engine shards.
 *
 * Each matching shard runs as an independent OS process communicating
 * via shared memory ring buffers. This enforces V6 isolation:
 *   1 shard = 1 process = 1 CPU core = 1 order book domain
 *
 * Layout of the shared memory segment per shard:
 *   [bt_shard_shmem_t header]
 *   [bt_match_in_queue_t  input queue]   (Sequencer → Shard)
 *   [bt_md_tick_queue_t   md queue]      (Shard → Market Data)
 *   [stats / control flags]
 */

typedef struct bt_shard_shmem {
    /* Control */
    uint32_t magic;              /* 0x53484152 = "SHAR" */
    uint32_t version;
    int      shard_id;
    char     shard_name[32];
    _Atomic int running;         /* 1 = active, 0 = stop requested */
    _Atomic int ready;           /* 1 = shard initialized and accepting */

    /* Statistics (updated by shard process, read by controller) */
    _Atomic uint64_t orders_received;
    _Atomic uint64_t orders_matched;
    _Atomic uint64_t trades_generated;
    _Atomic uint64_t total_qty;
    _Atomic double   total_notional;

    /* Health */
    _Atomic uint64_t heartbeat_ns; /* last heartbeat timestamp */

    /* Queue offset descriptors (relative to end of this header) */
    uint64_t input_queue_offset;
    uint64_t input_queue_size;
    uint64_t md_queue_offset;
    uint64_t md_queue_size;

    uint64_t create_time;
    uint64_t _pad[4];
} bt_shard_shmem_t;

#define BT_SHARD_MAGIC    0x53484152
#define BT_SHARD_VERSION  1

/* ── Shard Process Launcher ────────────────────────────────────────── */

typedef struct bt_shard_launcher bt_shard_launcher_t;

/**
 * Create a launcher for N matching shard processes.
 * @param num_shards Number of shards (= number of child processes)
 */
bt_shard_launcher_t *bt_shard_launcher_create(int num_shards);

/**
 * Start a specific shard process.
 * @param launcher    Launcher context
 * @param shard_id    Shard index (0..N-1)
 * @param cpu_core    CPU core to pin the shard process to
 * @param in_queue    Input queue (shared memory backed for IPC)
 * @param md_queue    Market data output queue
 * @param arena       Memory pool arena for this shard
 * @param journal     Journal handle (shared across processes)
 * @param event_bus   Event bus handle (shared memory backed for IPC)
 * @return PID of the child process, or -1 on failure
 */
int bt_shard_launcher_start_shard(bt_shard_launcher_t *launcher,
                                   int shard_id, int cpu_core,
                                   bt_match_in_queue_t *in_queue,
                                   bt_md_tick_queue_t *md_queue,
                                   bt_mempool_arena_t *arena,
                                   bt_journal_t *journal,
                                   bt_event_bus_t *event_bus);

/**
 * Check if a shard process is still alive.
 * @return 1 if alive, 0 if dead, -1 on error
 */
int bt_shard_launcher_is_alive(const bt_shard_launcher_t *launcher, int shard_id);

/**
 * Send stop signal to a shard process.
 */
void bt_shard_launcher_stop_shard(bt_shard_launcher_t *launcher, int shard_id);

/**
 * Wait for all shard processes to exit.
 */
void bt_shard_launcher_wait_all(bt_shard_launcher_t *launcher);

/**
 * Destroy the launcher and free resources.
 */
void bt_shard_launcher_destroy(bt_shard_launcher_t *launcher);

/* ── Shared memory helpers ─────────────────────────────────────────── */

/**
 * Create a shared memory segment for a shard's queues.
 * @param shard_id        Shard index
 * @param input_queue_sz  Size of input queue
 * @param md_queue_sz     Size of market data queue
 * @return Pointer to the shared memory header, or NULL on failure
 */
bt_shard_shmem_t *bt_shard_shmem_create(int shard_id,
                                         size_t input_queue_sz,
                                         size_t md_queue_sz);

/**
 * Attach to an existing shard shared memory segment.
 */
bt_shard_shmem_t *bt_shard_shmem_attach(int shard_id);

/**
 * Detach and optionally unlink a shard shared memory segment.
 */
void bt_shard_shmem_destroy(bt_shard_shmem_t *shm, int unlink_it);

#ifdef __cplusplus
}
#endif

#endif /* BT_SHARD_IPC_H */
