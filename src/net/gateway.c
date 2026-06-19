#define _GNU_SOURCE
#include "bt_types.h"
#include "bt_config.h"
#include "bt_queues.h"
#include "bt_timer.h"
#include "bt_cpu.h"
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
 */

/* ── Connection state ──────────────────────────────────────────────── */
typedef enum { GW_CONN_READING, GW_CONN_CLOSING } gw_conn_state_t;

typedef struct {
    int             fd;
    gw_conn_state_t state;
    uint8_t         recv_buf[BT_CFG_RECV_BUF_SIZE];
    size_t          recv_len;
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
    volatile int        running;
    bt_gw_oms_queue_t  *out_queue;
    int                 listen_fd;
    int                 epoll_fd;
    gw_conn_t          *conns;
    int                 num_conns;
    pthread_t           thread;
    uint64_t            orders_received;
} gw_ctx_t;

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

/* ── Parse order from payload ──────────────────────────────────────── */
static int gw_parse_order(const uint8_t *payload, size_t len, bt_order_request_t *req)
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

/* ── Accept new connection ─────────────────────────────────────────── */
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

        if (ctx->num_conns >= ctx->max_conns) { close(cfd); continue; }

        int opt = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        struct epoll_event ev;
        ev.events   = EPOLLIN | EPOLLET;
        ev.data.ptr = &ctx->conns[ctx->num_conns];

        if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            close(cfd); continue;
        }

        gw_conn_t *conn = &ctx->conns[ctx->num_conns];
        memset(conn, 0, sizeof(*conn));
        conn->fd               = cfd;
        conn->state            = GW_CONN_READING;
        conn->last_active      = bt_timer_now_ns();
        conn->rate_window_start = conn->last_active;
        ctx->num_conns++;
    }
}

/* ── Process received data ─────────────────────────────────────────── */
static void gw_process_data(gw_ctx_t *ctx, gw_conn_t *conn)
{
    while (conn->recv_len >= 5) {
        uint32_t msg_len;
        memcpy(&msg_len, conn->recv_buf, 4);
        msg_len = ntohl(msg_len);

        if (msg_len < 5 || msg_len > BT_CFG_RECV_BUF_SIZE) {
            conn->state = GW_CONN_CLOSING; return;
        }
        if (conn->recv_len < msg_len) break;

        uint8_t msg_type   = conn->recv_buf[4];
        uint8_t *payload   = conn->recv_buf + 5;
        size_t   payload_len = msg_len - 5;

        if (msg_type == 'O') {
            bt_order_request_t req;
            if (gw_parse_order(payload, payload_len, &req) == 0) {
                uint64_t now = bt_timer_now_ns();
                if (now - conn->rate_window_start > 1000000000UL) {
                    conn->rate_window_start = now; conn->rate_count = 0;
                }
                if (conn->rate_count < BT_CFG_RATE_LIMIT_RPS) {
                    conn->rate_count++;
                    bt_gw_oms_msg_t msg;
                    msg.request = req;
                    msg.seq_num = 0;
                    BT_MPSC_PUSH(*ctx->out_queue, msg);
                    ctx->orders_received++;
                }
            }
        }
        memmove(conn->recv_buf, conn->recv_buf + msg_len, conn->recv_len - msg_len);
        conn->recv_len -= msg_len;
    }
}

/* ── Gateway thread ────────────────────────────────────────────────── */
static void *gw_thread_core(void *arg)
{
    gw_ctx_t *ctx = (gw_ctx_t *)arg;
    bt_cpu_pin_thread(ctx->cpu_core);
    bt_cpu_set_realtime(60);

    fprintf(stderr, "[gateway-%d] port %d, core %d\n", ctx->thread_id, ctx->port, ctx->cpu_core);

    struct epoll_event events[256];
    while (ctx->running) {
        int nfds = epoll_wait(ctx->epoll_fd, events, 256, 1);
        if (nfds < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.ptr == NULL) {
                if (events[i].events & EPOLLIN) gw_accept(ctx);
            } else {
                gw_conn_t *conn = (gw_conn_t *)events[i].data.ptr;
                if (events[i].events & (EPOLLERR | EPOLLHUP)) conn->state = GW_CONN_CLOSING;

                if (events[i].events & EPOLLIN) {
                    while (1) {
                        ssize_t n = read(conn->fd, conn->recv_buf + conn->recv_len,
                                         BT_CFG_RECV_BUF_SIZE - conn->recv_len);
                        if (n > 0) { conn->recv_len += n; conn->last_active = bt_timer_now_ns();
                                     gw_process_data(ctx, conn); }
                        else if (n == 0) { conn->state = GW_CONN_CLOSING; break; }
                        else { if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                               conn->state = GW_CONN_CLOSING; break; }
                    }
                }
                if (conn->state == GW_CONN_CLOSING) {
                    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    close(conn->fd); conn->fd = -1;
                }
            }
        }
    }
    fprintf(stderr, "[gateway-%d] stopped. orders=%lu\n", ctx->thread_id, ctx->orders_received);
    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────── */
gw_ctx_t *bt_gateway_create(int thread_id, int cpu_core, int port,
                             int max_conns, bt_gw_oms_queue_t *out_queue)
{
    gw_ctx_t *ctx = (gw_ctx_t *)calloc(1, sizeof(gw_ctx_t));
    if (!ctx) return NULL;
    ctx->thread_id = thread_id; ctx->cpu_core = cpu_core;
    ctx->port = port; ctx->max_conns = max_conns;
    ctx->out_queue = out_queue; ctx->running = 1;

    ctx->conns = (gw_conn_t *)calloc(max_conns, sizeof(gw_conn_t));
    if (!ctx->conns) { free(ctx); return NULL; }
    for (int i = 0; i < max_conns; i++) ctx->conns[i].fd = -1;

    ctx->listen_fd = gw_create_listen(port);
    ctx->epoll_fd = epoll_create1(0);
    if (ctx->listen_fd < 0 || ctx->epoll_fd < 0) {
        if (ctx->listen_fd >= 0) close(ctx->listen_fd);
        if (ctx->epoll_fd >= 0) close(ctx->epoll_fd);
        free(ctx->conns); free(ctx); return NULL;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET; ev.data.ptr = NULL;
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->listen_fd, &ev);
    return ctx;
}

int bt_gateway_start(gw_ctx_t *ctx) {
    if (!ctx) return -1;
    return pthread_create(&ctx->thread, NULL, gw_thread_core, ctx);
}

void bt_gateway_stop(gw_ctx_t *ctx) {
    if (!ctx) return;
    ctx->running = 0; pthread_join(ctx->thread, NULL);
    for (int i = 0; i < ctx->max_conns; i++)
        if (ctx->conns[i].fd >= 0) close(ctx->conns[i].fd);
    if (ctx->listen_fd >= 0) close(ctx->listen_fd);
    if (ctx->epoll_fd >= 0) close(ctx->epoll_fd);
}

void bt_gateway_destroy(gw_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->running) bt_gateway_stop(ctx);
    free(ctx->conns); free(ctx);
}
