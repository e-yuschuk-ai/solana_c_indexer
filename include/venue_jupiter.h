/*
 * Jupiter v6 (ROADMAP.md milestone M6).
 *
 * Jupiter is an aggregator: it does not hold liquidity, it routes through the
 * pools that do. A route therefore contains the swaps that matter, one per leg,
 * and each leg is already recorded from the venue it went through. Emitting a
 * pool swap for the route as well would count every routed trade twice, which
 * is why `idx_venue_is_pool` is false here and the normalization step keeps
 * these rows off the pool series.
 *
 * What the route is worth reading for is attribution and reach:
 *
 *   - it says a trade was routed rather than sent to a pool directly, which is
 *     most of the retail flow on Solana;
 *   - its `SwapEvent` states the mints and the exact amounts of each leg,
 *     including legs through venues this indexer has no decoder for. That is
 *     the one place a swap on an unknown program becomes visible at all.
 *
 * The event's `amm` field is deliberately dropped. It is documented as the AMM
 * that filled the leg, but it is not consistently a pool address — a PumpSwap
 * leg carries the PumpSwap *program* id there — and a pool column that is
 * sometimes a program is worse than no column. The leg's own venue supplies
 * the pool when this indexer decodes it.
 */
#ifndef IDX_VENUE_JUPITER_H
#define IDX_VENUE_JUPITER_H

#include "block.h"
#include "error.h"
#include "venue.h"

/*
 * Decodes `ix` — an instruction of the Jupiter v6 program — into the swap its
 * event describes.
 *
 *   IDX_OK             `out` holds one leg of a route
 *   IDX_ERR_NOT_FOUND  not a swap event: the route instructions themselves,
 *                      the fee events, and everything else
 *   IDX_ERR_RANGE      a SwapEvent shorter than its fields
 */
idx_status idx_venue_jupiter_decode(const idx_transaction *tx,
                                    const idx_instruction *ix, idx_swap *out,
                                    idx_error *err);

#endif /* IDX_VENUE_JUPITER_H */
