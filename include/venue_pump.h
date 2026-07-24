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

/*
 * The structural accounts of a PumpSwap buy/sell instruction — the two mints
 * and the pool vaults, which its event does not carry. They are read from the
 * instruction rather than resolved from the trader's token accounts because
 * the quote-side account is often a wrapped-SOL account created and closed
 * within the same transaction, so it never reaches the block's token balances;
 * the mints, named outright as accounts 3 and 4, always do.
 */
typedef struct {
    idx_pubkey pool;
    idx_pubkey base_mint;
    idx_pubkey quote_mint;
    idx_pubkey user_base_account;  /* tells a resolved side which mint it is */
    idx_pubkey pool_base_vault;
    idx_pubkey pool_quote_vault;
} idx_pump_amm_accounts;

/*
 * Fills `out` and returns true when `ix` is a PumpSwap swap instruction (buy or
 * sell) naming enough accounts. The instruction, not its event, is where these
 * live; normalization pairs the two on the pool.
 */
bool idx_venue_pump_amm_accounts(const idx_transaction *tx,
                                 const idx_instruction *ix,
                                 idx_pump_amm_accounts *out);

#endif /* IDX_VENUE_PUMP_H */
