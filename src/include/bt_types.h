#ifndef BT_TYPES_H
#define BT_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdalign.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Compile-time constants ─────────────────────────────────────────── */
#define BT_MAX_SYMBOLS       1024
#define BT_MAX_SYMBOL_LEN    16
#define BT_MAX_ORDER_ID_LEN  32
#define BT_MAX_USER_ID_LEN   32
#define BT_PRICE_LEVELS_MAX  256
#define BT_CACHE_LINE_SIZE   64

/* ── Order Side ────────────────────────────────────────────────────── */
typedef enum {
    BT_SIDE_BUY  = 0,
    BT_SIDE_SELL = 1
} bt_side_t;

/* ── Order Type ────────────────────────────────────────────────────── */
typedef enum {
    BT_TYPE_LIMIT  = 0,
    BT_TYPE_MARKET = 1,
    BT_TYPE_IOC    = 2,  /* Immediate-or-Cancel */
    BT_TYPE_FOK    = 3   /* Fill-or-Kill       */
} bt_order_type_t;

/* ── Order Status ──────────────────────────────────────────────────── */
typedef enum {
    BT_STATUS_NEW        = 0,
    BT_STATUS_ACK        = 1,
    BT_STATUS_PARTIAL    = 2,
    BT_STATUS_FILLED     = 3,
    BT_STATUS_CANCELED   = 4,
    BT_STATUS_REJECTED   = 5
} bt_order_status_t;

/* ── Order (cache-line aligned, 64 bytes exactly) ──────────────────── */
typedef struct {
    uint64_t order_id;            /*  8 bytes */
    uint64_t user_id;             /*  8 bytes */
    char     symbol[16];          /* 16 bytes */
    double   price;               /*  8 bytes */
    uint32_t quantity;            /*  4 bytes */
    uint32_t filled_qty;          /*  4 bytes */
    uint64_t timestamp;           /*  8 bytes */
    uint8_t  side;                /*  1 byte  */
    uint8_t  type;                /*  1 byte  */
    uint8_t  status;              /*  1 byte  */
    uint8_t  _pad[5];             /*  5 bytes padding */
} bt_order_t;

/* compile-time size check */
#ifdef __cplusplus
static_assert(sizeof(bt_order_t) == 64, "bt_order_t must be 64 bytes");
#else
_Static_assert(sizeof(bt_order_t) == 64, "bt_order_t must be 64 bytes");
#endif

/* ── Trade Record ──────────────────────────────────────────────────── */
typedef struct {
    uint64_t trade_id;
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    uint64_t buy_user_id;         /* FIX (2026-06): for correct clearing */
    uint64_t sell_user_id;        /* FIX (2026-06): for correct clearing */
    double   price;
    uint32_t quantity;
    uint64_t timestamp;
} bt_trade_t;

/* ── Order Book Node (link in FIFO queue at a price level) ─────────── */
typedef struct bt_order_node {
    bt_order_t             order;
    struct bt_order_node  *next;
    struct bt_order_node  *prev;
} bt_order_node_t;

/* ── Price Level (FIFO queue of orders at same price) ──────────────── */
typedef struct bt_price_level {
    double            price;
    bt_order_node_t  *head;
    bt_order_node_t  *tail;
    uint32_t          order_count;
    uint64_t          total_quantity;
} bt_price_level_t;

/* ── Skip List Node for Price Levels ───────────────────────────────── */
#define BT_SKIPLIST_MAX_LEVEL 16

typedef struct bt_sl_node {
    double              price;
    bt_price_level_t   *level;
    struct bt_sl_node  *forward[1]; /* flexible array, actual size = level */
} bt_sl_node_t;

/* ── Order Book Snapshot ───────────────────────────────────────────── */
typedef struct {
    char     symbol[16];
    double   best_bid;
    double   best_ask;
    uint32_t bid_qty;
    uint32_t ask_qty;
    double   bid_levels[10];
    uint32_t bid_qtys[10];
    double   ask_levels[10];
    uint32_t ask_qtys[10];
    int      num_bid_levels;
    int      num_ask_levels;
    uint64_t timestamp;
    uint64_t seq_num;
} bt_order_book_snapshot_t;

/* ── Market Data Tick ──────────────────────────────────────────────── */
typedef enum {
    BT_TICK_TRADE       = 0,
    BT_TICK_BEST_BID    = 1,
    BT_TICK_BEST_ASK    = 2,
    BT_TICK_SNAPSHOT    = 3
} bt_tick_type_t;

typedef struct {
    bt_tick_type_t  type;
    char            symbol[16];
    double          price;
    uint32_t        quantity;
    uint64_t        timestamp;
    uint64_t        trade_id;
    uint8_t         side;       /* for trade ticks */
    uint8_t         _pad[7];
} bt_md_tick_t;

/* ── Order Request (from gateway to OMS) ───────────────────────────── */
typedef struct {
    uint64_t        request_id;
    uint64_t        user_id;
    char            symbol[16];
    double          price;
    uint32_t        quantity;
    bt_side_t       side;
    bt_order_type_t type;
    uint64_t        timestamp;
} bt_order_request_t;

/* ── Order Response (back to gateway) ──────────────────────────────── */
typedef struct {
    uint64_t          request_id;
    uint64_t          order_id;
    bt_order_status_t status;
    uint32_t          filled_qty;
    char              reject_reason[64];
} bt_order_response_t;

/* ── Risk Check Result ─────────────────────────────────────────────── */
typedef struct {
    uint64_t order_id;
    int      passed;       /* 1 = pass, 0 = fail */
    char     reason[64];   /* reason if failed */
} bt_risk_result_t;

/* ── System statistics ─────────────────────────────────────────────── */
typedef struct {
    uint64_t orders_received;
    uint64_t orders_matched;
    uint64_t orders_rejected;
    uint64_t orders_canceled;
    uint64_t trades_generated;
    uint64_t total_quantity_traded;
    double   total_notional_traded;
    uint64_t risk_blocks;
    uint64_t seq_num;
} bt_stats_t;

#ifdef __cplusplus
}
#endif

#endif /* BT_TYPES_H */
