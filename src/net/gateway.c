#define _GNU_SOURCE
#include "bt_types.h"
#include "bt_config.h"
#include "bt_queues.h"
#include "bt_timer.h"
#include "bt_cpu.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* ── TCP Gateway ──────────────────────────────────────────────────────
 * High-performance TCP server using epoll edge-triggered I/O.
 * Output: pushes order requests to bt_gw_oms_queue_t.
 *
 * Optimizations (2026-06):
 *  - Connection pool reuse: closed slots (fd==-1) are recycled.
 *  - Ring-buffer recv: circular buffer eliminates O(n²) memmove.
 *  - Binary protocol fast-path (msg type 'B'): direct struct copy
 *    avoids expensive string parsing for high-throughput clients.
 */

/* ── Connection state ──────────────────────────────────────────────── */
typedef enum { GW_CONN_READING, GW_CONN_CLOSING } gw_conn_state_t;

typedef struct {
    int             fd;
    gw_conn_state_t state;
    uint8_t         recv_buf[BT_CFG_RECV_BUF_SIZE];
    size_t          recv_head;       /* ring-buffer read cursor */
    size_t          recv_tail;       /* ring-buffer write cursor (bytes stored) */
    uint64_t        last_active;
    uint64_t        rate_window_start;
    int             rate_count;
} gw_conn_t;

/* ── Gateway context ───────────────────────────────────────────────── */
typedef struct {
    int                 thread_id;
    int                 cpu_core;
    int                 port;
    int                 max_conns;
    atomic_int          running;
    bt_gw_oms_queue_t  *out_queue;
    int                 listen_fd;
    int                 epoll_fd;
    gw_conn_t          *conns;
    int                 active_conns;  /* actual active connections */
    pthread_t           thread;
    uint64_t            orders_received;
} gw_ctx_t;

/* ── Ring-buffer helpers ───────────────────────────────────────────── */

/* bytes available to read from ring buffer */
static inline size_t gw_ring_readable(const gw_conn_t *c)
{
    if (c->recv_tail >= c->recv_head)
        return c->recv_tail - c->recv_head;
    else
        return BT_CFG_RECV_BUF_SIZE - c->recv_head + c->recv_tail;
}

/* space left for writing into ring buffer */
static inline size_t gw_ring_writable(const gw_conn_t *c)
{
    return (BT_CFG_RECV_BUF_SIZE - 1) - gw_ring_readable(c);
}

/* copy `len` bytes from ring buffer at offset `off` (from head) into dst,
 * wrapping as needed. `off + len` must not exceed readable bytes. */
static inline void gw_ring_peek(const gw_conn_t *c, size_t off,
                                 uint8_t *dst, size_t len)
{
    size_t pos = (c->recv_head + off) & (BT_CFG_RECV_BUF_SIZE - 1);
    size_t first = BT_CFG_RECV_BUF_SIZE - pos;
    if (first >= len) {
        memcpy(dst, c->recv_buf + pos, len);
    } else {
        memcpy(dst, c->recv_buf + pos, first);
        memcpy(dst + first, c->recv_buf, len - first);
    }
}

/* advance head by `len` bytes (discard consumed data) */
static inline void gw_ring_consume(gw_conn_t *c, size_t len)
{
    c->recv_head = (c->recv_head + len) & (BT_CFG_RECV_BUF_SIZE - 1);
}

/* ── Create listen socket ──────────────────────────────────────────── */
static int gw_create_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

/* ── Find a free connection slot (reuses closed slots) ─────────────── */
static gw_conn_t *gw_find_free_slot(gw_ctx_t *ctx)
{
    /* First, scan for a closed slot to reuse */
    for (int i = 0; i < ctx->max_conns; i++) {
        if (ctx->conns[i].fd == -1)
            return &ctx->conns[i];
    }
    return NULL; /* all slots occupied */
}

/* ── Parse text-protocol order (FIX-lite: key=value|...) ───────────── */
static int gw_parse_order_text(const uint8_t *payload, size_t len,
                                bt_order_request_t *req)
{
    memset(req, 0, sizeof(*req));
    req->timestamp = bt_timer_now_ns();

    const char *p = (const char *)payload;
    const char *end = p + len;

    while (p < end) {
        while (p < end && (*p == '|' || *p == ' ')) p++;
        if (p >= end) break;

        const char *eq = (const char *)memchr(p, '=', end - p);
        if (!eq) break;
        const char *val_start = eq + 1;
        const char *pipe = (const char *)memchr(val_start, '|', end - val_start);
        const char *val_end = pipe ? pipe : end;

        size_t key_len = eq - p;
        size_t val_len = val_end - val_start;

        char val_buf[32];
        size_t copy_len = val_len < 31 ? val_len : 31;
        memcpy(val_buf, val_start, copy_len);
        val_buf[copy_len] = '\0';

        if (key_len == 1 && p[0] == 'u') {
            req->user_id = strtoull(val_buf, NULL, 10);
        } else if (key_len == 1 && p[0] == 's') {
            size_t s_len = val_len < 15 ? val_len : 15;
            memcpy(req->symbol, val_start, s_len);
            req->symbol[s_len] = '\0';
        } else if (key_len == 1 && p[0] == 'p') {
            req->price = strtod(val_buf, NULL);
        } else if (key_len == 1 && p[0] == 'q') {
            req->quantity = (uint32_t)strtoul(val_buf, NULL, 10);
        } else if (key_len == 1 && p[0] == 'd') {
            req->side = (val_buf[0] == 'S' || val_buf[0] == 's') ? BT_SIDE_SELL : BT_SIDE_BUY;
        } else if (key_len == 1 && p[0] == 't') {
            switch (val_buf[0]) {
                case 'L': req->type = BT_TYPE_LIMIT;  break;
                case 'M': req->type = BT_TYPE_MARKET; break;
                case 'I': req->type = BT_TYPE_IOC;    break;
                case 'F': req->type = BT_TYPE_FOK;    break;
                default:  req->type = BT_TYPE_LIMIT;  break;
            }
        }
        p = val_end;
    }
    return req->symbol[0] != '\0' ? 0 : -1;
}

/* ── Parse binary-protocol order (type 'B': raw bt_order_request_t) ────
 * Binary protocol format:
 *   [4B msg_len (net order)] [1B type='B'] [bt_order_request_t struct]
 * The bt_order_request_t fields are in native byte order (matching the
 * server's endianness — typically little-endian on x86_64).
 * Field layout (see bt_types.h): request_id, user_id, symbol[16], price,
 * quantity, side, type, timestamp. */
static int gw_parse_order_binary(const uint8_t *payload, size_t len,
                                  bt_order_request_t *req)
{
    /* Minimal payload: at least request_id + user_id + symbol[16] +
     * price + quantity + side(1) + type(1) + timestamp + padding */
    if (len < 48) return -1;  /* too short for binary order */
    /* Copy directly from wire — client must use matching struct layout */
    memcpy(req, payload, sizeof(bt_order_request_t));
    /* Overwrite timestamp with server-side arrival time for accuracy */
    req->timestamp = bt_timer_now_ns();
    /* Basic sanity check */
    if (req->symbol[0] == '\0') return -1;
    if (req->quantity == 0) return -1;
    return 0;
}

/* ── Push a validated order request to the MPSC output queue ───────── */
static inline void gw_push_order(gw_ctx_t *ctx, gw_conn_t *conn,
                                  const bt_order_request_t *req)
{
    uint64_t now = bt_timer_now_ns();
    if (now - conn->rate_window_start > 1000000000UL) {
        conn->rate_window_start = now;
        conn->rate_count = 0;
    }
    if (conn->rate_count < BT_CFG_RATE_LIMIT_RPS) {
        conn->rate_count++;
        bt_gw_oms_msg_t msg;
        msg.request = *req;
        msg.seq_num = 0;
        BT_MPSC_PUSH(*ctx->out_queue, msg);
        ctx->orders_received++;
    }
}

/* ── Accept new connection (with slot reuse) ───────────────────────── */
static void gw_accept(gw_ctx_t *ctx)
{
    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept4(ctx->listen_fd, (struct sockaddr *)&caddr, &clen, SOCK_NONBLOCK);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }

        /* Try to reuse a closed slot first */
        gw_conn_t *conn = gw_find_free_slot(ctx);
        if (!conn) {
            /* All slots occupied — reject new connection */
            close(cfd);
            continue;
        }

        int opt = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        struct epoll_event ev;
        ev.events   = EPOLLIN | EPOLLET;
        ev.data.ptr = conn;

        if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            close(cfd);
            continue;
        }

        memset(conn, 0, sizeof(*conn));
        conn->fd               = cfd;
        conn->state            = GW_CONN_READING;
        conn->recv_head        = 0;
        conn->recv_tail        = 0;
        conn->last_active      = bt_timer_now_ns();
        conn->rate_window_start = conn->last_active;
        ctx->active_conns++;
    }
}

/* ── Process received data (ring-buffer edition) ──────────────────────
 * Reads complete messages from the ring buffer without memmove.
 * After each message is processed, head advances (O(1)).              */
static void gw_process_data(gw_ctx_t *ctx, gw_conn_t *conn)
{
    while (gw_ring_readable(conn) >= 5) {
        /* Peek 4-byte length prefix (network byte order) */
        uint32_t msg_len_raw;
        gw_ring_peek(conn, 0, (uint8_t *)&msg_len_raw, 4);
        uint32_t msg_len = ntohl(msg_len_raw);

        if (msg_len < 5 || msg_len > BT_CFG_RECV_BUF_SIZE) {
            conn->state = GW_CONN_CLOSING;
            return;
        }

        /* Need full message? */
        if (gw_ring_readable(conn) < msg_len) break;

        /* Peek message type */
        uint8_t msg_type;
        gw_ring_peek(conn, 4, &msg_type, 1);

        size_t payload_len = msg_len - 5;

        if (msg_type == 'O' || msg_type == 'B') {
            /* Extract payload into a temporary buffer.
             * For binary ('B'), we could zero-copy from the ring, but
             * payload is at most 64KB (BT_CFG_RECV_BUF_SIZE) and the
             * common case is a single message per read anyway. */
            uint8_t payload_buf[BT_CFG_RECV_BUF_SIZE];
            if (payload_len <= sizeof(payload_buf)) {
                gw_ring_peek(conn, 5, payload_buf, payload_len);

                bt_order_request_t req;
                int ok = (msg_type == 'B')
                    ? gw_parse_order_binary(payload_buf, payload_len, &req)
                    : gw_parse_order_text(payload_buf, payload_len, &req);

                if (ok == 0) {
                    gw_push_order(ctx, conn, &req);
                }
            }
        }

        /* Consume the message (O(1) — no memmove) */
        gw_ring_consume(conn, msg_len);
    }
}

/* ── Gateway (edge-triggered read loop) ────────────────────────────── */
static void *gw_thread_core(void *arg)
{
    gw_ctx_t *ctx = (gw_ctx_t *)arg;
    bt_cpu_pin_thread(ctx->cpu_core);
    bt_cpu_set_realtime(60);

    fprintf(stderr, "[gateway-%d] port %d, core %d\n",
            ctx->thread_id, ctx->port, ctx->cpu_core);

    struct epoll_event events[256];
    while (atomic_load(&ctx->running)) {
        int nfds = epoll_wait(ctx->epoll_fd, events, 256, 1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.ptr == NULL) {
                if (events[i].events & EPOLLIN) gw_accept(ctx);
            } else {
                gw_conn_t *conn = (gw_conn_t *)events[i].data.ptr;
                if (events[i].events & (EPOLLERR | EPOLLHUP))
                    conn->state = GW_CONN_CLOSING;

                if (events[i].events & EPOLLIN) {
                    while (1) {
                        /* Ring-buffer write: read directly into tail position.
                         * Limit to (a) contiguous physical space from tail to buf end,
                         * and (b) logical free space before we'd overwrite head. */
                        size_t tail   = conn->recv_tail;
                        size_t phys   = BT_CFG_RECV_BUF_SIZE - tail; /* bytes to buf end */
                        size_t free   = gw_ring_writable(conn);
                        size_t toread = phys < free ? phys : free;
                        if (toread == 0) break; /* buffer full — wait for consumer */
                        ssize_t n = read(conn->fd, conn->recv_buf + tail, toread);
                        if (n > 0) {
                            conn->recv_tail = (tail + (size_t)n) & (BT_CFG_RECV_BUF_SIZE - 1);
                            conn->last_active = bt_timer_now_ns();
                            gw_process_data(ctx, conn);
                        } else if (n == 0) {
                            conn->state = GW_CONN_CLOSING;
                            break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK)
                                break;
                            conn->state = GW_CONN_CLOSING;
                            break;
                        }
                    }
                }
                if (conn->state == GW_CONN_CLOSING) {
                    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    close(conn->fd);
                    conn->fd = -1;  /* mark slot as free for reuse */
                    ctx->active_conns--;
                }
            }
        }
    }
    fprintf(stderr, "[gateway-%d] stopped. orders=%lu\n",
            ctx->thread_id, ctx->orders_received);
    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────── */
gw_ctx_t *bt_gateway_create(int thread_id, int cpu_core, int port,
                             int max_conns, bt_gw_oms_queue_t *out_queue)
{
    gw_ctx_t *ctx = (gw_ctx_t *)calloc(1, sizeof(gw_ctx_t));
    if (!ctx) return NULL;
    ctx->thread_id  = thread_id;
    ctx->cpu_core   = cpu_core;
    ctx->port       = port;
    ctx->max_conns  = max_conns;
    ctx->out_queue  = out_queue;
    atomic_init(&ctx->running, 1);

    ctx->conns = (gw_conn_t *)calloc((size_t)max_conns, sizeof(gw_conn_t));
    if (!ctx->conns) { free(ctx); return NULL; }
    for (int i = 0; i < max_conns; i++) ctx->conns[i].fd = -1;

    ctx->listen_fd = gw_create_listen(port);
    ctx->epoll_fd  = epoll_create1(0);
    if (ctx->listen_fd < 0 || ctx->epoll_fd < 0) {
        if (ctx->listen_fd >= 0) close(ctx->listen_fd);
        if (ctx->epoll_fd >= 0)  close(ctx->epoll_fd);
        free(ctx->conns); free(ctx); return NULL;
    }

    struct epoll_event ev;
    ev.events   = EPOLLIN | EPOLLET;
    ev.data.ptr = NULL;
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->listen_fd, &ev);
    return ctx;
}

int bt_gateway_start(gw_ctx_t *ctx) {
    if (!ctx) return -1;
    return pthread_create(&ctx->thread, NULL, gw_thread_core, ctx);
}

void bt_gateway_stop(gw_ctx_t *ctx) {
    if (!ctx) return;
    atomic_store(&ctx->running, 0);
    pthread_join(ctx->thread, NULL);
    for (int i = 0; i < ctx->max_conns; i++)
        if (ctx->conns[i].fd >= 0) close(ctx->conns[i].fd);
    if (ctx->listen_fd >= 0) close(ctx->listen_fd);
    if (ctx->epoll_fd >= 0)  close(ctx->epoll_fd);
}

void bt_gateway_destroy(gw_ctx_t *ctx) {
    if (!ctx) return;
    if (atomic_load(&ctx->running)) bt_gateway_stop(ctx);
    free(ctx->conns); free(ctx);
}
