#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>
#include "bt_config.h"

/* ── Simple Ring-Buffer Logger ────────────────────────────────────────
 *
 * Non-blocking logger for the hot path.
 * Messages are stored in a ring buffer, flushed periodically by a
 * background thread. printf in the hot path is avoided.
 */

#define LOG_RING_SIZE  4096
#define LOG_MSG_SIZE   256

typedef struct {
    char     msg[LOG_MSG_SIZE];
    int      level;
} log_entry_t;

static log_entry_t   log_ring[LOG_RING_SIZE];
static _Atomic int   log_head = 0;
static _Atomic int   log_tail = 0;
static int           log_level = 2;  /* 0=err, 1=warn, 2=info, 3=debug */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE         *log_file = NULL;

void bt_log_set_level(int level)
{
    log_level = level;
}

void bt_log_set_file(const char *path)
{
    pthread_mutex_lock(&log_mutex);
    if (log_file && log_file != stderr) fclose(log_file);
    log_file = fopen(path, "a");
    if (!log_file) log_file = stderr;
    pthread_mutex_unlock(&log_mutex);
}

/* Ring-buffer log — safe to call from any thread including hot path */
void bt_log(int level, const char *fmt, ...)
{
    if (level > log_level) return;

    int tail = atomic_load(&log_tail);
    int next = (tail + 1) % LOG_RING_SIZE;

    if (next == atomic_load(&log_head)) {
        /* Buffer full — drop oldest */
        atomic_store(&log_head, (atomic_load(&log_head) + 1) % LOG_RING_SIZE);
    }

    log_entry_t *entry = &log_ring[tail];

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    int prefix_len = snprintf(entry->msg, LOG_MSG_SIZE,
                              "%02d:%02d:%02d.%03ld [%d] ",
                              tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                              ts.tv_nsec / 1000000, level);

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry->msg + prefix_len, LOG_MSG_SIZE - prefix_len, fmt, args);
    va_end(args);

    entry->level = level;
    atomic_store(&log_tail, next);
}

/* Flush pending log messages to output */
void bt_log_flush(void)
{
    int head = atomic_load(&log_head);
    int tail = atomic_load(&log_tail);

    pthread_mutex_lock(&log_mutex);
    FILE *out = log_file ? log_file : stderr;

    while (head != tail) {
        fputs(log_ring[head].msg, out);
        fputc('\n', out);
        head = (head + 1) % LOG_RING_SIZE;
    }
    fflush(out);

    atomic_store(&log_head, head);
    pthread_mutex_unlock(&log_mutex);
}

void bt_log_init(void)
{
    log_file = stderr;
}
