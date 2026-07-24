/*
 * Swap normalization (ROADMAP.md milestone M6).
 *
 * A venue decoder (venue.h) says what one instruction states about a trade,
 * which is deliberately partial: an event carries amounts but not mints, an
 * instruction carries accounts and one amount but not the other. This turns
 * each into a complete row — both mints, both amounts, both scales, the pool
 * and the trader — resolving what the venue left open against what the block
 * already carried, and never fetching anything (decision D5).
 *
 * Three resolutions happen here:
 *
 *   Amounts   from the most authoritative source the venue offers (D9): the
 *             program's own event, else Raydium's ray_log, else the change in
 *             the pool's vaults across the transaction.
 *   Mints     from the token balances in meta for the accounts the venue
 *             named. A token account states its mint there; a raw amount plus
 *             a mint is a swap a price series can use.
 *   Decimals  the same balances carry them, and both sides' scales ride on the
 *             row so the price step multiplies without another join.
 *
 * Attribution is per invocation, not per transaction. Each swap instruction
 * names its own accounts, so a route through three pools yields three rows,
 * one per pool, each resolved against that pool's own accounts — which is why
 * the vault deltas, per transaction though they are, still attribute correctly
 * when a transaction touches several pools, as long as no single pool is
 * touched twice (D9 covers that case with the per-invocation sources).
 *
 * Jupiter is not a pool (D8): its route is netted to the endpoints the wallet
 * actually paid and received, and recorded as one completed trade rather than
 * as swaps of its own — those are already the legs' pool rows.
 */
#ifndef IDX_SWAP_H
#define IDX_SWAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "block.h"
#include "error.h"
#include "types.h"
#include "venue.h"

/* Where a row's amounts came from, ranked by D9. Kept on the row because "how
 * do we know" is worth being able to ask of a price that looks wrong. */
typedef enum {
    IDX_AMOUNT_NONE = 0,
    IDX_AMOUNT_EVENT,  /* the program's own CPI event */
    IDX_AMOUNT_RAYLOG, /* Raydium's ray_log line */
    IDX_AMOUNT_DELTA   /* the change in a pool vault across the transaction */
} idx_amount_source;

const char *idx_amount_source_name(idx_amount_source source);

typedef enum {
    IDX_SWAP_POOL = 0,  /* a trade against one pool */
    IDX_SWAP_AGGREGATED /* a completed route: what the wallet paid and got */
} idx_swap_row_kind;

/*
 * One normalized swap. `input` is the side that left the trader, `output` the
 * side that reached them, each a mint, a raw amount and the mint's decimals.
 * A field that could not be resolved leaves its `has_` flag false rather than
 * guessing: a row that names one mint and one amount is still worth keeping,
 * and the price step decides what it can do with a partial one.
 *
 * The instruction path is completed by the caller from the slot and
 * transaction index it holds; here only the position within the transaction
 * is known.
 */
typedef struct {
    idx_swap_row_kind kind;
    idx_venue venue;
    idx_amount_source source;

    uint16_t instruction_index;
    uint16_t inner_index;
    bool inner;

    idx_pubkey pool; /* the token mint for a curve trade, see the source */
    bool has_pool;
    idx_pubkey user;
    bool has_user;

    idx_pubkey input_mint;
    bool has_input_mint;
    uint64_t input_amount;
    bool has_input_amount;
    uint8_t input_decimals;
    bool has_input_decimals;

    idx_pubkey output_mint;
    bool has_output_mint;
    uint64_t output_amount;
    bool has_output_amount;
    uint8_t output_decimals;
    bool has_output_decimals;
} idx_swap_row;

/*
 * Normalizes the swaps of `tx` into an array allocated from `arena`, in
 * execution order, with the aggregated route row (if any) last.
 *
 * A failed transaction and one fetched without metadata yield nothing, the
 * same as the balance and transfer extractors: without the token balances
 * there is nothing to resolve against, and a rolled-back swap did not happen.
 *
 *   IDX_OK             `out` and `out_count` are set, possibly to NULL and 0
 *   IDX_ERR_NO_MEMORY  the arena could not grow
 *   IDX_ERR_RANGE      a payload a venue decoder recognised is truncated
 *   IDX_ERR_PARSE      such an instruction names too few accounts
 *
 * The last two are the venue decoder disagreeing with a swap the chain ran,
 * which is a bug here rather than bad data, so they are reported. An
 * instruction of an unknown program, or a known program's non-swap, is
 * skipped.
 */
idx_status idx_swap_normalize(const idx_transaction *tx, idx_arena *arena,
                              const idx_swap_row **out, size_t *out_count,
                              idx_error *err);

#endif /* IDX_SWAP_H */
