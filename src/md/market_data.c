#include "bt_types.h"
#include "bt_config.h"
#include "bt_queues.h"
#include "bt_timer.h"
#include "bt_cpu.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── Market Data Engine ───────────────────────────────────────────────
 * Receives trade ticks from matching engine via bt_md_tick_queue_t.
 */

typedef struct {
    int                thread_id, cpu_core;
    atomic_int          running;
    bt_md_tick_queue_t *in_queue;
    pthread_t          thread;
    uint64_t           tick_count;
} bt_md_ctx_t;

static void *md_thread(void *arg)
{
    bt_md_ctx_t *ctx = (bt_md_ctx_t *)arg;
    bt_cpu_pin_thread(ctx->cpu_core); bt_cpu_set_realtime(50);
    fprintf(stderr, "[md] core %d\n", ctx->cpu_core);

    while (atomic_load(&ctx->running)) {
        bt_md_tick_t tick;
        if (!BT_SPSC_POP(*ctx->in_queue, &tick)) { BT_CPU_PAUSE(); continue; }
        ctx->tick_count++;
    }
    fprintf(stderr, "[md] stopped. ticks=%lu\n", ctx->tick_count);
    return NULL;
}

bt_md_ctx_t *bt_md_create(int tid, int cpu, bt_md_tick_queue_t *in)
{
    bt_md_ctx_t *ctx = (bt_md_ctx_t *)calloc(1, sizeof(bt_md_ctx_t));
    if (!ctx) return NULL;
    ctx->thread_id = tid; ctx->cpu_core = cpu; ctx->in_queue = in; atomic_init(&ctx->running, 1);
    return ctx;
}
int  bt_md_start(bt_md_ctx_t *ctx) { return ctx ? pthread_create(&ctx->thread, NULL, md_thread, ctx) : -1; }
void bt_md_stop(bt_md_ctx_t *ctx)  { if (ctx) { atomic_store(&ctx->running, 0); pthread_join(ctx->thread, NULL); } }
void bt_md_destroy(bt_md_ctx_t *ctx) { if (ctx) { if (atomic_load(&ctx->running)) bt_md_stop(ctx); free(ctx); } }
