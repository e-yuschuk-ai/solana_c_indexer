/*
 * Bar (OHLCV) derivation (ROADMAP.md milestone M6, decision D5).
 *
 * A bar is the price series a terminal draws: for one pool and one time bucket,
 * the open, high, low and close of the price and the volume that traded. D5
 * fixes the shape — two resolutions, 1s and 1m, keyed `(pool, bucket)`, never
 * aggregated across pools — and where the numbers come from: swaps priced
 * against a quote mint (price.h). A swap with no price contributes nothing, and
 * a route is not a pool (D8) so it contributes nothing either.
 *
 * Price and volume are the ones the price step produced: the price is quote
 * units per base unit, the base and quote volumes are the swap's amounts scaled
 * by their decimals. A bar's quote is the pool's, which is fixed, so every swap
 * in a bucket agrees on what the price means.
 *
 * Open and close are by execution order, not arrival order. Blocks can commit
 * out of order — a backfilled slot arrives after a later one (D6) — so each swap
 * carries a sequence (slot, then its position in the block), and the earliest
 * sets the open while the latest sets the close, whichever showed up first.
 * That also makes derivation a pure fold over the swaps, which is what lets the
 * reorg path recompute a bucket by re-folding the swaps that survive (bar.c).
 *
 * Like the other dimensions this accumulates across blocks and owns its records;
 * it belongs to the processing thread (D6) and is not thread-safe. It is what M7
 * will back with the bar tables and flush on bucket close; until then it lives
 * in the process.
 */
#ifndef IDX_BAR_H
#define IDX_BAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "map.h"
#include "price.h"
#include "types.h"

typedef enum {
    IDX_BAR_1S = 0,
    IDX_BAR_1M,
    IDX_BAR_INTERVAL_COUNT
} idx_bar_interval;

/* Lowercase name ("1s", "1m"); never NULL. */
const char *idx_bar_interval_name(idx_bar_interval interval);

/* The interval's width in seconds (1, 60). */
uint32_t idx_bar_interval_seconds(idx_bar_interval interval);

/*
 * Where a swap sits in execution order, for assigning open and close under
 * out-of-order arrival. Compared lexicographically: slot, then transaction,
 * then the instruction path, with a top-level instruction ahead of the inner
 * ones it expands into.
 */
typedef struct {
    idx_slot slot;
    uint16_t transaction_index;
    uint16_t instruction_index;
    uint16_t inner_index;
    bool inner;
} idx_bar_seq;

/* <0, 0 or >0 as `a` orders before, with or after `b`. */
int idx_bar_seq_compare(const idx_bar_seq *a, const idx_bar_seq *b);

/*
 * One priced swap, as the bar builder consumes it. `price` must have
 * `has_price`; `pool` is the pool it traded against (a row with no pool, or an
 * aggregated route, is not a bar input). `block_time` is the block's unix
 * seconds, which is the only timestamp the chain offers — every swap in a block
 * shares it.
 */
typedef struct {
    idx_pubkey pool;
    idx_price price;
    int64_t block_time;
    idx_bar_seq seq;
} idx_bar_input;

/* One OHLCV bar. `bucket` is the unix second the interval starts at. */
typedef struct {
    idx_pubkey pool;
    idx_bar_interval interval;
    int64_t bucket;
    idx_quote quote; /* what the price is denominated in */

    double open;
    double high;
    double low;
    double close;
    double base_volume;  /* base units traded, scaled by decimals */
    double quote_volume; /* quote units traded, scaled by decimals */
    uint32_t swap_count;

    idx_bar_seq open_seq;  /* the earliest swap, which set `open` */
    idx_bar_seq close_seq; /* the latest, which set `close` */
} idx_bar;

typedef struct {
    idx_map by_key; /* pool+interval+bucket -> idx_bar* (heap-owned) */
} idx_bar_registry;

void idx_bar_registry_init(idx_bar_registry *reg);
void idx_bar_registry_free(idx_bar_registry *reg);

/*
 * Folds one priced swap into its 1s and 1m bars. An input without a price, or
 * without a block time to bucket by, is ignored.
 *
 *   IDX_OK             folded (including the ignored cases)
 *   IDX_ERR_NO_MEMORY  the registry could not grow
 */
idx_status idx_bar_registry_observe(idx_bar_registry *reg,
                                    const idx_bar_input *in, idx_error *err);

/* The bar for `(pool, interval, bucket)`, or NULL. Borrows the registry. */
const idx_bar *idx_bar_registry_get(const idx_bar_registry *reg,
                                    const idx_pubkey *pool,
                                    idx_bar_interval interval, int64_t bucket);

/* Bars held, across all pools, intervals and buckets. */
size_t idx_bar_registry_count(const idx_bar_registry *reg);

#endif /* IDX_BAR_H */
