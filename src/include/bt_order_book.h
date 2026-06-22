#ifndef BT_ORDER_BOOK_H
#define BT_ORDER_BOOK_H

#include "bt_types.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Order Book ────────────────────────────────────────────────────────
 *
 * Per-symbol order book with price-time priority matching.
 * Uses skip lists for O(log N) price level insertion and lookup.
 * Bids are sorted descending (highest price first).
 * Asks are sorted ascending (lowest price first).
 *
 * ── C interface (callable from C++ matching engine) ──────────────────
 */

typedef struct bt_order_book bt_order_book_t;

/**
 * Create a new order book for a symbol.
 * @param symbol  Symbol identifier (will be copied)
 * @return New order book, or NULL on failure
 */
bt_order_book_t *bt_order_book_create(const char *symbol);

/**
 * Destroy an order book and free all associated memory.
 */
void bt_order_book_destroy(bt_order_book_t *book);

/**
 * Insert a limit order into the book.
 * Caller provides a pre-allocated order_node (from memory pool).
 * Returns 0 on success, -1 on duplicate.
 */
int bt_order_book_insert(bt_order_book_t *book, bt_order_node_t *node);

/**
 * Cancel an order by ID. Removes from book if unfilled.
 * Returns the order node (to be recycled) or NULL if not found.
 */
bt_order_node_t *bt_order_book_cancel(bt_order_book_t *book, uint64_t order_id);

/**
 * Match an incoming aggressive order against the book.
 * Fills generate trade records pushed to the output arrays.
 *
 * @param book       Order book
 * @param side       Aggressor side (BUY aggresses asks, SELL aggresses bids)
 * @param price      Limit price (0 for market orders)
 * @param quantity   Requested quantity
 * @param type       Order type (LIMIT, MARKET, IOC, FOK)
 * @param order_id   Aggressor order ID
 * @param user_id    Aggressor user ID
 * @param trades_out Output array for generated trades
 * @param max_trades Max number of trades to generate
 * @param num_trades Output: actual number of trades generated
 * @return Remaining unfilled quantity (0 = fully filled)
 */
uint32_t bt_order_book_match(bt_order_book_t *book,
                              bt_side_t side,
                              double price,
                              uint32_t quantity,
                              bt_order_type_t type,
                              uint64_t order_id,
                              uint64_t user_id,
                              bt_trade_t *trades_out,
                              int max_trades,
                              int *num_trades);

/**
 * Get the best bid price (0 if no bids).
 */
double bt_order_book_best_bid(const bt_order_book_t *book);

/**
 * Get the best ask price (0 if no asks).
 */
double bt_order_book_best_ask(const bt_order_book_t *book);

/**
 * Get a snapshot of the order book (top N levels).
 */
void bt_order_book_snapshot(const bt_order_book_t *book,
                             bt_order_book_snapshot_t *snap,
                             int max_levels,
                             uint64_t seq_num);

/**
 * Get total order count in the book.
 */
int bt_order_book_order_count(const bt_order_book_t *book);

/**
 * Get the symbol of this order book.
 */
const char *bt_order_book_symbol(const bt_order_book_t *book);

#ifdef __cplusplus
}
#endif

#endif /* BT_ORDER_BOOK_H */
