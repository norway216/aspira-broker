#ifndef BT_CLEARING_H
#define BT_CLEARING_H

#include "bt_types.h"
#include "bt_event.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Clearing & Settlement Engine ────────────────────────────────────
 * V5: Processes trades and updates account balances.
 *
 * Subscribes to the Event Bus for TRADE_EXECUTED events.
 * Maintains a ledger of all clearing operations.
 *
 * Key functions:
 *  - Trade settlement: transfer asset from seller to buyer
 *  - Account balance updates
 *  - Fee calculation
 *  - Netting (simplified: immediate settlement per trade)
 */

#define BT_CLEARING_MAX_ACCOUNTS 65536

/* ── Account ───────────────────────────────────────────────────────── */
typedef struct {
    uint64_t account_id;
    char     symbol[16];        /* asset symbol */
    double   balance;           /* available balance */
    double   frozen;            /* locked/frozen balance */
    double   total_traded;      /* cumulative traded notional */
    uint64_t trade_count;       /* number of trades */
    uint64_t last_update_ns;    /* last activity timestamp */
} bt_account_t;

/* ── Ledger Entry (double-entry accounting) ────────────────────────── */
typedef struct {
    uint64_t entry_id;
    uint64_t trade_id;
    uint64_t debit_account;     /* account losing value */
    uint64_t credit_account;    /* account gaining value */
    double   amount;
    double   fee;
    uint64_t timestamp;
    char     asset[16];
} bt_ledger_entry_t;

/* ── Clearing context ─────────────────────────────────────────────── */
typedef struct bt_clearing bt_clearing_t;

/**
 * Create a clearing engine.
 * @param thread_id  Thread identifier
 * @param cpu_core   CPU core to pin to
 * @return Clearing handle, or NULL on failure
 */
bt_clearing_t *bt_clearing_create(int thread_id, int cpu_core);

/**
 * Start the clearing engine thread.
 * The engine subscribes to the event bus and processes TRADE_EXECUTED events.
 * @param ctx  Clearing engine
 * @param bus  Event bus to subscribe to
 * @return 0 on success
 */
int bt_clearing_start(bt_clearing_t *ctx, bt_event_bus_t *bus);

/**
 * Stop the clearing engine and wait for completion.
 */
void bt_clearing_stop(bt_clearing_t *ctx);

/**
 * Destroy the clearing engine.
 */
void bt_clearing_destroy(bt_clearing_t *ctx);

/**
 * Get clearing statistics.
 */
void bt_clearing_stats(const bt_clearing_t *ctx,
                        uint64_t *trades_settled, double *total_notional,
                        uint64_t *ledger_entries);

/**
 * Query an account balance.
 * @return 0 if found, -1 if account not found
 */
int bt_clearing_get_account(const bt_clearing_t *ctx, uint64_t account_id,
                             const char *symbol, bt_account_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BT_CLEARING_H */
