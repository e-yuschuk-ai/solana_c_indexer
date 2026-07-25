/*
 * Pricing a swap (ROADMAP.md milestone M6, decisions D5 and D10).
 *
 * A swap row (swap.h) states both mints and both raw amounts, but a raw amount
 * is a number of base units and says nothing about value on its own. A price
 * appears only when one side of the trade is a mint whose value is already
 * known — a quote mint: SOL/WSOL, or a dollar stablecoin. Then the other side,
 * the base, is priced in that quote, and the row gains the one number a bar is
 * built from.
 *
 * The quote set is configurable and ordered by priority (decision D10). A trade
 * between two quote mints — a SOL/USDC pool, say — is priced in whichever quote
 * ranks higher, so the base is the lower-priority side; the default order puts
 * the dollar stables ahead of SOL, so such a pool reports SOL's price in
 * dollars rather than the reverse. A trade with no quote side is left unpriced:
 * still a swap, just no price (D5).
 *
 * Nothing here is fetched. The mints, amounts and decimals all come from the
 * swap row, which resolved them from what the block carried (D5); this step
 * only recognises the quote and does the arithmetic D9 deferred:
 *
 *     price = (quote_raw / 10^quote_dec) / (base_raw / 10^base_dec)
 *
 * expressed as quote units per one whole base unit.
 */
#ifndef IDX_PRICE_H
#define IDX_PRICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "swap.h"
#include "types.h"

/*
 * Which quote a priced side is, kept on the result so "what is this price
 * denominated in" is answerable without re-matching the mint. IDX_QUOTE_OTHER
 * is a mint the configuration named by address with no well-known label.
 */
typedef enum {
    IDX_QUOTE_NONE = 0, /* not a quote mint, or a swap that was not priced */
    IDX_QUOTE_SOL,      /* native SOL / wrapped SOL */
    IDX_QUOTE_USDC,
    IDX_QUOTE_USDT,
    IDX_QUOTE_USD1,
    IDX_QUOTE_OTHER
} idx_quote;

/* Lowercase name ("sol", "usdc", "other"), for logs. Never NULL. */
const char *idx_quote_name(idx_quote quote);

/* The most quote mints a configuration may name. Far above the four defaults,
 * but bounded so the set is a fixed-size value that copies freely. */
#define IDX_QUOTE_SET_MAX 16

typedef struct {
    idx_pubkey mint;
    idx_quote kind;
} idx_quote_mint;

/*
 * An ordered set of quote mints. Order is priority: the entry nearest the front
 * wins when a swap touches two of them, which is the tie-break D10 describes.
 */
typedef struct {
    idx_quote_mint mints[IDX_QUOTE_SET_MAX];
    size_t count;
} idx_quote_set;

/* The built-in default set: usdc, usdt, usd1, sol, in that priority order. */
void idx_quote_set_defaults(idx_quote_set *set);

/*
 * Parses a comma-separated list into `set`, in the order given, which is the
 * priority order. Each entry is either a well-known name (sol/wsol, usdc, usdt,
 * usd1, case-insensitively) or a base58 mint address, which becomes an
 * IDX_QUOTE_OTHER entry. An empty list is valid and prices nothing. Repeated
 * mints keep their first, highest-priority position.
 *
 *   IDX_OK             `set` is populated
 *   IDX_ERR_PARSE      an entry is neither a name nor a valid base58 key
 *   IDX_ERR_RANGE      more than IDX_QUOTE_SET_MAX distinct mints, or an entry
 *                      longer than a base58 pubkey can be
 */
idx_status idx_quote_set_parse(idx_quote_set *set, const char *spec,
                               idx_error *err);

/*
 * The price of one swap, or the fact that it has none. `priced` is set when one
 * side is a quote mint; `has_price` is set when a number could actually be
 * computed from it, which additionally needs both amounts, both decimals, and a
 * non-zero base amount. A row can be priced but numberless — a quote side whose
 * decimals the block never carried — which is worth distinguishing from a swap
 * that simply has no quote side.
 */
typedef struct {
    bool priced;
    idx_quote quote; /* which quote the quote side is */

    idx_pubkey quote_mint;
    idx_pubkey base_mint;
    uint64_t quote_amount;
    uint64_t base_amount;
    uint8_t quote_decimals;
    uint8_t base_decimals;

    bool has_price;
    double price; /* quote units per one whole base unit */
} idx_price;

/*
 * Prices `row` against `set`, writing the outcome to `out`. Returns true when
 * the row was priced (one side is a quote mint), false otherwise — including
 * for NULL arguments, in which case `out` is left untouched only if it is
 * itself NULL. `out` is always fully written when non-NULL.
 *
 * Works on any row kind: an aggregated route row is priced on its netted
 * endpoints, which is the price between what the wallet paid and received (D8).
 */
bool idx_price_of_swap(const idx_quote_set *set, const idx_swap_row *row,
                       idx_price *out);

#endif /* IDX_PRICE_H */
