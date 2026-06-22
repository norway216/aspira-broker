/* ── TCP Gateway ──────────────────────────────────────────────────────
 * High-performance TCP server using epoll edge-triggered I/O.
 * Output: pushes order/cancel requests to bt_gw_oms_queue_t.
 *
 * Features (2026-06 Round 4):
 *  - Connection pool reuse, ring-buffer recv (Round 1)
 *  - Binary protocol fast-path 'B' (Round 1)
 *  - Idle timeout, 8KB stack buffer (Round 3)
 *  - API key auth (type 'A') (Round 4)
 *  - Cancel requests (type 'C') (Round 4)
 *  - Client response send path (EPOLLOUT + ring buffer) (Round 4) */

#define _GNU_SOURCE
#include "bt_types.h"
#include "bt_config.h"
#include "bt_queues.h"
#include "bt_timer.h"
#include "bt_cpu.h"
#include "bt_scheduler.h"
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

#define GW_IDLE_TIMEOUT_NS   60000000000UL
#define GW_SEND_RING_SIZE    65536
#define GW_IDEM_RING_SIZE    8192       /* power of 2, O(1) idempotency */

/* ── Simple API key whitelist (demo/R&D) ──────────────────────────── */
#define BT_API_KEY_LEN  32
static const char *g_api_keys[] = {
    "test-key-00000000000000000000000",
    "benchmark-key-000000000000000000",
    NULL
};
static int gw_validate_api_key(const char *key)
{
    if (!key || !key[0]) return 0;
    for (int i = 0; g_api_keys[i]; i++)
        if (strncmp(key, g_api_keys[i], BT_API_KEY_LEN) == 0) return 1;
    return 0;
}

/* ── Connection state ──────────────────────────────────────────────── */
typedef enum { GW_CONN_READING, GW_CONN_CLOSING } gw_conn_state_t;

typedef struct {
    int             fd;
    gw_conn_state_t state;
    /* Recv ring buffer */
    uint8_t         recv_buf[BT_CFG_RECV_BUF_SIZE];
    size_t          recv_head, recv_tail;
    /* Send ring buffer (for responses to client) */
    uint8_t         send_buf[GW_SEND_RING_SIZE];
    size_t          send_head, send_tail;
    int             send_pending;    /* 1 = EPOLLOUT registered */
    /* Auth */
    int             authenticated;
    uint64_t        auth_user_id;
    /* Rate / timeout */
    uint64_t        last_active;
    uint64_t        rate_window_start;
    int             rate_count;
} gw_conn_t;

/* ── Gateway context ───────────────────────────────────────────────── */
typedef struct {
    int                   thread_id, cpu_core, port, max_conns;
    atomic_int            running;
    bt_gw_oms_queue_t    *out_queue;
    bt_gw_response_queue_t *response_queue;  /* responses from matching engine */
    int                   listen_fd, epoll_fd;
    gw_conn_t            *conns;
    int                   active_conns;
    pthread_t             thread;
    uint64_t              orders_received;
    /* V11: O(1) idempotency ring */
    uint64_t              idem_ring[GW_IDEM_RING_SIZE];
    int                   idem_idx;
    int sched_id;
} gw_ctx_t;

/* ── Ring-buffer helpers ───────────────────────────────────────────── */
static inline size_t gw_ring_readable(const uint8_t *buf, size_t head, size_t tail,
                                       size_t mask) {
    return (tail >= head) ? (tail - head) : (mask + 1 - head + tail);
}
static inline size_t gw_ring_writable(const uint8_t *buf, size_t head, size_t tail,
                                       size_t mask) {
    return mask - gw_ring_readable(buf, head, tail, mask);
}
static inline void gw_ring_peek(const uint8_t *buf, size_t head, size_t off,
                                 uint8_t *dst, size_t len, size_t mask) {
    size_t pos = (head + off) & mask;
    size_t first = (mask + 1) - pos;
    if (first >= len) memcpy(dst, buf + pos, len);
    else { memcpy(dst, buf + pos, first); memcpy(dst + first, buf, len - first); }
}
static inline size_t gw_ring_advance(size_t cursor, size_t delta, size_t mask) {
    return (cursor + delta) & mask;
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
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, SOMAXCONN) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

/* ── Text protocol parser ─────────────────────────────────────────── */
static int gw_parse_order_text(const uint8_t *payload, size_t len, bt_order_request_t *req)
{
    memset(req, 0, sizeof(*req)); req->timestamp = bt_timer_now_ns();
    const char *p = (const char *)payload, *end = p + len;
    while (p < end) {
        while (p < end && (*p == '|' || *p == ' ')) p++;
        if (p >= end) break;
        const char *eq = (const char *)memchr(p, '=', end - p); if (!eq) break;
        const char *vs = eq + 1;
        const char *pipe = (const char *)memchr(vs, '|', end - vs);
        const char *ve = pipe ? pipe : end;
        size_t kl = eq - p, vl = ve - vs;
        char vb[32]; size_t cl = vl < 31 ? vl : 31;
        memcpy(vb, vs, cl); vb[cl] = '\0';
        if (kl == 1 && p[0] == 'u') req->user_id = strtoull(vb, NULL, 10);
        else if (kl == 1 && p[0] == 's') { size_t sl = vl < 15 ? vl : 15; memcpy(req->symbol, vs, sl); req->symbol[sl] = '\0'; }
        else if (kl == 1 && p[0] == 'p') req->price = strtod(vb, NULL);
        else if (kl == 1 && p[0] == 'q') req->quantity = (uint32_t)strtoul(vb, NULL, 10);
        else if (kl == 1 && p[0] == 'd') req->side = (vb[0] == 'S' || vb[0] == 's') ? BT_SIDE_SELL : BT_SIDE_BUY;
        else if (kl == 1 && p[0] == 't') { switch (vb[0]) { case 'L': req->type = BT_TYPE_LIMIT; break; case 'M': req->type = BT_TYPE_MARKET; break; case 'I': req->type = BT_TYPE_IOC; break; case 'F': req->type = BT_TYPE_FOK; break; default: req->type = BT_TYPE_LIMIT; break; } }
        p = ve;
    }
    return req->symbol[0] ? 0 : -1;
}

static int gw_parse_order_binary(const uint8_t *payload, size_t len, bt_order_request_t *req)
{
    /* V10: fixed-size binary protocol validation (~56 bytes with padding) */
    if (len < 48 || len > sizeof(bt_order_request_t)) return -1;
    memcpy(req, payload, sizeof(bt_order_request_t));
    req->timestamp = bt_timer_now_ns();
    if (!req->symbol[0] || !req->quantity) return -1;
    if (req->side != BT_SIDE_BUY && req->side != BT_SIDE_SELL) return -1;
    if (req->type > BT_TYPE_FOK) return -1;
    return 0;
}

/* ── Push to output ───────────────────────────────────────────────── */
static inline void gw_push_order(gw_ctx_t *ctx, gw_conn_t *conn, const bt_order_request_t *req)
{
    uint64_t now = bt_timer_now_ns();
    if (now - conn->rate_window_start > 1000000000UL) { conn->rate_window_start = now; conn->rate_count = 0; }
    if (conn->rate_count < BT_CFG_RATE_LIMIT_RPS) {
        /* V11: O(1) idempotency — direct-index ring buffer.
         * request_id modulo RING_SIZE indexes into the ring.
         * Collision (different ID maps to same slot) → false reject is possible
         * but extremely rare with 8192 slots and random IDs. */
        if (req->request_id != 0) {
            int slot = (int)(req->request_id & (GW_IDEM_RING_SIZE - 1));
            if (ctx->idem_ring[slot] == req->request_id) return; /* duplicate */
            ctx->idem_ring[slot] = req->request_id;
        }
        conn->rate_count++;
        bt_gw_oms_msg_t msg; memset(&msg, 0, sizeof(msg));
        msg.msg_type = 'O'; msg.request = *req; msg.seq_num = 0;
        BT_MPSC_PUSH(*ctx->out_queue, msg); ctx->orders_received++;
    }
}

/* ── Send a response back to a specific connection ─────────────────── */
static void gw_send_to_conn(gw_conn_t *conn, const bt_order_response_t *resp)
{
    size_t mask = GW_SEND_RING_SIZE - 1;
    if (gw_ring_writable(conn->send_buf, conn->send_head, conn->send_tail, mask) < sizeof(*resp) + 5)
        return; /* send buffer full — drop */

    /* Wire format: [4B len][1B 'R'][bt_order_response_t] */
    uint32_t msg_len = htonl((uint32_t)(5 + sizeof(*resp)));
    uint8_t  type    = 'R';
    size_t pos = conn->send_tail;
    /* Write length */
    for (size_t i = 0; i < 4; i++)
        conn->send_buf[(pos + i) & mask] = ((uint8_t *)&msg_len)[i];
    conn->send_buf[(pos + 4) & mask] = type;
    /* Write response body */
    const uint8_t *rb = (const uint8_t *)resp;
    for (size_t i = 0; i < sizeof(*resp); i++)
        conn->send_buf[(pos + 5 + i) & mask] = rb[i];
    conn->send_tail = (pos + 5 + sizeof(*resp)) & mask;
}

/* ── Drain outgoing data from send ring buffer ─────────────────────── */
static void gw_flush_send(gw_conn_t *conn)
{
    size_t mask = GW_SEND_RING_SIZE - 1;
    while (conn->send_head != conn->send_tail) {
        size_t avail = gw_ring_readable(conn->send_buf, conn->send_head, conn->send_tail, mask);
        size_t pos = conn->send_head;
        size_t first = (mask + 1) - pos;
        size_t to_write = (first < avail) ? first : avail;
        ssize_t n = send(conn->fd, conn->send_buf + pos, to_write, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) conn->send_head = (pos + (size_t)n) & mask;
        else { if (errno == EAGAIN || errno == EWOULDBLOCK) break; conn->state = GW_CONN_CLOSING; break; }
    }
}

/* ── Process received data ─────────────────────────────────────────── */
static void gw_process_data(gw_ctx_t *ctx, gw_conn_t *conn)
{
    size_t rmask = BT_CFG_RECV_BUF_SIZE - 1;
    while (gw_ring_readable(conn->recv_buf, conn->recv_head, conn->recv_tail, rmask) >= 5) {
        uint32_t msg_len_raw;
        gw_ring_peek(conn->recv_buf, conn->recv_head, 0, (uint8_t *)&msg_len_raw, 4, rmask);
        uint32_t msg_len = ntohl(msg_len_raw);
        if (msg_len < 5 || msg_len > BT_CFG_RECV_BUF_SIZE) { conn->state = GW_CONN_CLOSING; return; }
        if (gw_ring_readable(conn->recv_buf, conn->recv_head, conn->recv_tail, rmask) < msg_len) break;

        uint8_t msg_type; gw_ring_peek(conn->recv_buf, conn->recv_head, 4, &msg_type, 1, rmask);
        size_t payload_len = msg_len - 5;

        /* ── Type 'A': API Key Authentication ────────────────────── */
        if (msg_type == 'A') {
            uint8_t small[256], *pb = small; int nf = 0;
            if (payload_len > sizeof(small)) { pb = (uint8_t *)malloc(payload_len); if (!pb) { conn->state = GW_CONN_CLOSING; return; } nf = 1; }
            gw_ring_peek(conn->recv_buf, conn->recv_head, 5, pb, payload_len, rmask);
            if (gw_validate_api_key((const char *)pb)) {
                conn->authenticated = 1;
                conn->auth_user_id  = 0; /* derive from key in production */
            }
            if (nf) free(pb);
            conn->recv_head = gw_ring_advance(conn->recv_head, msg_len, rmask);
            continue;
        }

        /* ── All trading messages require authentication ─────────── */
        if (!conn->authenticated) {
            conn->recv_head = gw_ring_advance(conn->recv_head, msg_len, rmask);
            continue;
        }

        /* ── Type 'O' or 'B': New Order ──────────────────────────── */
        if (msg_type == 'O' || msg_type == 'B') {
            uint8_t sbuf[8192], *pb = sbuf; int nf = 0;
            if (payload_len > sizeof(sbuf)) { pb = (uint8_t *)malloc(payload_len); if (!pb) { conn->state = GW_CONN_CLOSING; return; } nf = 1; }
            gw_ring_peek(conn->recv_buf, conn->recv_head, 5, pb, payload_len, rmask);
            bt_order_request_t req;
            int ok = (msg_type == 'B') ? gw_parse_order_binary(pb, payload_len, &req) : gw_parse_order_text(pb, payload_len, &req);
            if (ok == 0) gw_push_order(ctx, conn, &req);
            if (nf) free(pb);
            conn->recv_head = gw_ring_advance(conn->recv_head, msg_len, rmask);
            continue;
        }

        /* ── Type 'C': Cancel Request ────────────────────────────── */
        if (msg_type == 'C') {
            if (payload_len >= 32) {
                uint8_t cbuf[64];
                gw_ring_peek(conn->recv_buf, conn->recv_head, 5, cbuf, payload_len < 64 ? payload_len : 64, rmask);
                bt_cancel_request_t cr; memset(&cr, 0, sizeof(cr));
                memcpy(&cr.order_id, cbuf, 8);
                memcpy(&cr.user_id,  cbuf + 8, 8);
                memcpy(cr.symbol,    cbuf + 16, 16);
                cr.timestamp = bt_timer_now_ns();
                bt_gw_oms_msg_t msg; memset(&msg, 0, sizeof(msg));
                msg.msg_type = 'C'; msg.cancel = cr; msg.seq_num = 0;
                BT_MPSC_PUSH(*ctx->out_queue, msg);
            }
            conn->recv_head = gw_ring_advance(conn->recv_head, msg_len, rmask);
            continue;
        }

        /* Unknown type — skip */
        conn->recv_head = gw_ring_advance(conn->recv_head, msg_len, rmask);
    }
}

/* ── Pool responses from matching engine and dispatch to connections ── */
static void gw_drain_responses(gw_ctx_t *ctx)
{
    bt_order_response_t resp;
    while (BT_MPSC_POP(*ctx->response_queue, &resp)) {
        /* Route response by request_id: send to the connection
         * whose auth_user_id matches the request's origin.
         * Simplified: broadcast to all authenticated connections
         * for demo purposes (no request_id→conn mapping yet). */
        for (int i = 0; i < ctx->max_conns; i++) {
            if (ctx->conns[i].fd >= 0 && ctx->conns[i].authenticated)
                gw_send_to_conn(&ctx->conns[i], &resp);
        }
    }
}

/* ── Accept + gateway thread ───────────────────────────────────────── */
static gw_conn_t *gw_find_free_slot(gw_ctx_t *ctx) {
    for (int i = 0; i < ctx->max_conns; i++)
        if (ctx->conns[i].fd == -1) return &ctx->conns[i];
    return NULL;
}

static void gw_accept(gw_ctx_t *ctx) {
    while (1) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        int cfd = accept4(ctx->listen_fd, (struct sockaddr *)&ca, &cl, SOCK_NONBLOCK);
        if (cfd < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; break; }
        gw_conn_t *conn = gw_find_free_slot(ctx);
        if (!conn) { close(cfd); continue; }
        int opt = 1; setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        struct epoll_event ev; ev.events = EPOLLIN | EPOLLET; ev.data.ptr = conn;
        if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, cfd, &ev) < 0) { close(cfd); continue; }
        memset(conn, 0, sizeof(*conn));
        conn->fd = cfd; conn->state = GW_CONN_READING;
        conn->last_active = conn->rate_window_start = bt_timer_now_ns();
        ctx->active_conns++;
    }
}

static void *gw_thread_core(void *arg) {
    gw_ctx_t *ctx = (gw_ctx_t *)arg;
    bt_sched_apply(&g_sched, ctx->sched_id);
    fprintf(stderr, "[gateway-%d] port %d, core %d\n", ctx->thread_id, ctx->port, ctx->cpu_core);
    struct epoll_event events[256];
    while (__atomic_load_n(&ctx->running, __ATOMIC_RELAXED)) {
        /* Drain responses from matching engine */
        gw_drain_responses(ctx);
        int nfds = epoll_wait(ctx->epoll_fd, events, 256, 1);
        if (nfds < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < nfds; i++) {
            if (!events[i].data.ptr) { if (events[i].events & EPOLLIN) gw_accept(ctx); }
            else {
                gw_conn_t *conn = (gw_conn_t *)events[i].data.ptr;
                if (events[i].events & (EPOLLERR | EPOLLHUP)) conn->state = GW_CONN_CLOSING;
                if (events[i].events & EPOLLIN) {
                    while (1) {
                        size_t tail = conn->recv_tail, rmask = BT_CFG_RECV_BUF_SIZE - 1;
                        size_t phys = BT_CFG_RECV_BUF_SIZE - tail;
                        size_t free = gw_ring_writable(conn->recv_buf, conn->recv_head, tail, rmask);
                        size_t tr = phys < free ? phys : free;
                        if (!tr) break;
                        ssize_t n = read(conn->fd, conn->recv_buf + tail, tr);
                        if (n > 0) { conn->recv_tail = (tail + (size_t)n) & rmask; conn->last_active = bt_timer_now_ns(); gw_process_data(ctx, conn); }
                        else if (!n) { conn->state = GW_CONN_CLOSING; break; }
                        else { if (errno == EAGAIN || errno == EWOULDBLOCK) break; conn->state = GW_CONN_CLOSING; break; }
                    }
                }
                if (events[i].events & EPOLLOUT) gw_flush_send(conn);
                /* Re-register EPOLLOUT if send buffer has pending data */
                if (conn->send_head != conn->send_tail && !conn->send_pending) {
                    struct epoll_event ev; ev.events = EPOLLIN | EPOLLOUT | EPOLLET; ev.data.ptr = conn;
                    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
                    conn->send_pending = 1;
                } else if (conn->send_head == conn->send_tail && conn->send_pending) {
                    struct epoll_event ev; ev.events = EPOLLIN | EPOLLET; ev.data.ptr = conn;
                    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
                    conn->send_pending = 0;
                }
                if (conn->state == GW_CONN_CLOSING) {
                    gw_flush_send(conn); /* flush pending data before close */
                    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    close(conn->fd); conn->fd = -1; ctx->active_conns--;
                }
            }
        }
        /* Idle timeout scan */
        uint64_t now = bt_timer_now_ns();
        for (int c = 0; c < ctx->max_conns; c++)
            if (ctx->conns[c].fd >= 0 && now - ctx->conns[c].last_active > GW_IDLE_TIMEOUT_NS)
                ctx->conns[c].state = GW_CONN_CLOSING;
    }
    fprintf(stderr, "[gateway-%d] stopped. orders=%lu\n", ctx->thread_id, ctx->orders_received);
    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────── */
gw_ctx_t *bt_gateway_create(int tid, int cpu, int port, int max_conns, int sched_id,
                             bt_gw_oms_queue_t *out,
                             bt_gw_response_queue_t *response_queue) {
    gw_ctx_t *ctx = (gw_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->thread_id = tid; ctx->cpu_core = cpu; ctx->sched_id = sched_id; ctx->port = port; ctx->max_conns = max_conns;
    ctx->out_queue = out; ctx->response_queue = response_queue;
    __atomic_store_n(&ctx->running, 1, __ATOMIC_RELAXED);
    ctx->conns = (gw_conn_t *)calloc((size_t)max_conns, sizeof(gw_conn_t));
    if (!ctx->conns) { free(ctx); return NULL; }
    for (int i = 0; i < max_conns; i++) ctx->conns[i].fd = -1;
    ctx->listen_fd = gw_create_listen(port); ctx->epoll_fd = epoll_create1(0);
    if (ctx->listen_fd < 0 || ctx->epoll_fd < 0) {
        if (ctx->listen_fd >= 0) close(ctx->listen_fd);
        if (ctx->epoll_fd >= 0)  close(ctx->epoll_fd);
        free(ctx->conns); free(ctx); return NULL;
    }
    struct epoll_event ev; ev.events = EPOLLIN | EPOLLET; ev.data.ptr = NULL;
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->listen_fd, &ev);
    return ctx;
}
int  bt_gateway_start(gw_ctx_t *ctx) { return ctx ? pthread_create(&ctx->thread, NULL, gw_thread_core, ctx) : -1; }
void bt_gateway_stop(gw_ctx_t *ctx) {
    if (!ctx) return;
    __atomic_store_n(&ctx->running, 0, __ATOMIC_RELAXED); pthread_join(ctx->thread, NULL);
    for (int i = 0; i < ctx->max_conns; i++)
        if (ctx->conns[i].fd >= 0) close(ctx->conns[i].fd);
    if (ctx->listen_fd >= 0) close(ctx->listen_fd);
    if (ctx->epoll_fd >= 0)  close(ctx->epoll_fd);
}
void bt_gateway_destroy(gw_ctx_t *ctx) {
    if (!ctx) return;
    if (__atomic_load_n(&ctx->running, __ATOMIC_RELAXED)) bt_gateway_stop(ctx);
    free(ctx->conns); free(ctx);
}
