/*
 * pump.fun: the bonding curve and PumpSwap (ROADMAP.md milestone M6).
 *
 * A meme token has two lives here. It is born on the bonding curve, where the
 * counter-side is native SOL held by the curve itself, and once the curve
 * fills the token graduates to PumpSwap, an ordinary constant-product AMM with
 * a base and a quote vault. They are two venues, not one, because a pool is
 * what D5 keys bars by and the curve and the pool are different accounts with
 * different reserves — the price series has a seam there whether or not this
 * decoder draws it.
 *
 * Both programs are Anchor and both emit a CPI event on every trade, and that
 * event is what is decoded. The instruction is not:
 *
 *   - the account layout is not stable. The curve's trades arrive in two
 *     shapes today — one of 17-18 accounts with the mint third, one of 26-28
 *     with the mint second — and a program upgrade adds more. The event's
 *     leading fields have not moved.
 *   - the event states what was traded; the instruction states what was
 *     requested. `buy` names the tokens wanted and a maximum cost, and the
 *     cost is what a price series needs.
 *
 * Only the prefix of each event is read, and the rest is ignored the way the
 * rest of this codebase ignores trailing instruction bytes: pump's TradeEvent
 * has grown twice and ends in a Borsh string, so anything that depended on its
 * total length would already be broken.
 */
#ifndef IDX_VENUE_PUMP_H
#define IDX_VENUE_PUMP_H

#include "block.h"
#include "error.h"
#include "venue.h"

/*
 * Decodes `ix` — an instruction of the curve or of PumpSwap, as `venue` says —
 * into the swap its event describes.
 *
 *   IDX_OK             `out` holds a swap
 *   IDX_ERR_NOT_FOUND  not a trade: any instruction of these programs that is
 *                      not one of their trade events, which is most of them
 *   IDX_ERR_RANGE      an event this recognised is shorter than its fields
 */
idx_status idx_venue_pump_decode(const idx_transaction *tx,
                                 const idx_instruction *ix, idx_venue venue,
                                 idx_swap *out, idx_error *err);

#endif /* IDX_VENUE_PUMP_H */
