#ifndef BT_EVENT_H
#define BT_EVENT_H

#include "bt_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Event Sourcing System ────────────────────────────────────────────
 * V5: Formal event types for full event-sourced architecture.
 *
 * Every state-changing operation produces an immutable event.
 * Events flow through the Event Bus to downstream consumers:
 *   - Market Data (tick generation)
 *   - Persistence (journal)
 *   - Clearing & Settlement
 *   - Audit trail
 *   - Replay engine (future)
 */

typedef enum {
    BT_EVENT_ORDER_CREATED   = 0,
    BT_EVENT_ORDER_REJECTED  = 1,
    BT_EVENT_ORDER_MATCHED   = 2,
    BT_EVENT_TRADE_EXECUTED  = 3,
    BT_EVENT_ORDER_CANCELED  = 4,
    BT_EVENT_SNAPSHOT        = 5,
    BT_EVENT_CIRCUIT_BREAKER = 6,
    BT_EVENT_KILL_SWITCH     = 7,
} bt_event_type_t;

/* ── Event structure (64-byte aligned) ─────────────────────────────── */
typedef struct {
    bt_event_type_t  type;           /* event type */
    uint64_t         seq;            /* global sequence number */
    uint64_t         timestamp;      /* event timestamp (ns) */

    union {
        /* ORDER_CREATED / ORDER_REJECTED */
        bt_order_t        order;

        /* ORDER_MATCHED */
        struct {
            uint64_t       order_id;
            uint32_t       matched_qty;
            uint32_t       remaining_qty;
            double         match_price;
        } order_matched;

        /* TRADE_EXECUTED */
        bt_trade_t        trade;

        /* ORDER_CANCELED */
        struct {
            uint64_t       order_id;
            char           reason[32];
        } order_canceled;

        /* CIRCUIT_BREAKER / KILL_SWITCH */
        struct {
            char           details[48];
        } control;
    } data;

    uint8_t          _pad[8];        /* pad */
} bt_event_t;

/* Ensure proper size */
#ifdef __cplusplus
static_assert(sizeof(bt_event_t) <= 128, "bt_event_t must fit in 128 bytes");
#else
_Static_assert(sizeof(bt_event_t) <= 128, "bt_event_t must fit in 128 bytes");
#endif

/* ── Event callback type ───────────────────────────────────────────── */
typedef void (*bt_event_handler_t)(const bt_event_t *event, void *user_data);

/* ── Event Bus ───────────────────────────────────────────────────────
 * Multi-producer, multi-consumer event distribution.
 * Consumers register handlers for specific event types.
 * Producers push events; the bus fans out to all matching handlers.
 */

typedef struct bt_event_bus bt_event_bus_t;

/**
 * Create a new event bus.
 * @param capacity  Maximum number of pending events in the ring buffer
 * @return Event bus handle, or NULL on failure
 */
bt_event_bus_t *bt_event_bus_create(size_t capacity);

/**
 * Register a handler for one or more event types.
 * @param bus     Event bus
 * @param types   Bitmask of bt_event_type_t values to subscribe to
 * @param handler Callback function
 * @param user_data Opaque user data passed to handler
 * @return Handler ID (for unregister), or -1 on failure
 */
int bt_event_bus_subscribe(bt_event_bus_t *bus, uint32_t type_mask,
                            bt_event_handler_t handler, void *user_data);

/**
 * Unsubscribe a previously registered handler.
 * Safe to call from any thread — the handler will not be invoked
 * after this call returns.
 * @param bus        Event bus
 * @param handler_id Handler ID returned by bt_event_bus_subscribe
 * @return 0 on success, -1 if invalid ID
 */
int bt_event_bus_unsubscribe(bt_event_bus_t *bus, int handler_id);

/**
 * Publish an event to all registered handlers.
 * Non-blocking: returns immediately, handlers are called synchronously.
 * @return Number of handlers that received the event
 */
int bt_event_bus_publish(bt_event_bus_t *bus, const bt_event_t *event);

/**
 * Destroy the event bus and free resources.
 */
void bt_event_bus_destroy(bt_event_bus_t *bus);

/**
 * Get event bus statistics.
 */
void bt_event_bus_stats(const bt_event_bus_t *bus,
                         uint64_t *published, uint64_t *delivered);

/* ── Event factory helpers ─────────────────────────────────────────── */
bt_event_t bt_event_order_created(const bt_order_t *order, uint64_t seq);
bt_event_t bt_event_order_rejected(const bt_order_request_t *req, const char *reason, uint64_t seq);
bt_event_t bt_event_trade_executed(const bt_trade_t *trade, uint64_t seq);
bt_event_t bt_event_order_canceled(uint64_t order_id, const char *reason, uint64_t seq);

#ifdef __cplusplus
}
#endif

#endif /* BT_EVENT_H */
