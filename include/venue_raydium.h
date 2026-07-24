/*
 * Raydium (ROADMAP.md milestone M6).
 *
 * Raydium is the venue that shows why the swap row cannot be read off the
 * instruction alone. Its AMM v4 is not an Anchor program and emits no event:
 * a swap states one exact amount and a limit on the other, so half of every
 * row has to come from what actually moved through the pool's vaults. That is
 * the case D5 designs for, and what this decoder produces is the naming the
 * normalization step needs — the pool, its two vaults, the trader's two token
 * accounts, and whichever amount the instruction fixed.
 *
 * Two programs are covered:
 *
 *   AMM v4    the constant-product pools, a one-byte discriminant and a packed
 *             payload. `SwapBaseIn` fixes what goes in, `SwapBaseOut` what
 *             comes out.
 *   CLMM      concentrated liquidity, Anchor, `swap` and `swapV2`. The
 *             direction is a flag in the payload rather than two instructions,
 *             and `swapV2` names the two vault mints outright, which is the
 *             one Raydium shape that needs nothing resolved.
 *
 * The account layouts are taken from instructions observed on mainnet rather
 * than from an IDL alone, and an instruction whose account count is not one of
 * the shapes below is skipped rather than guessed at: naming the wrong account
 * as a vault would attribute someone else's balance delta to this pool.
 */
#ifndef IDX_VENUE_RAYDIUM_H
#define IDX_VENUE_RAYDIUM_H

#include "block.h"
#include "error.h"
#include "venue.h"

/*
 * Decodes `ix` — an instruction of one of the Raydium programs, as `venue`
 * says — into what it states about the swap.
 *
 *   IDX_OK             `out` holds a swap
 *   IDX_ERR_NOT_FOUND  not a swap instruction, or a swap whose account layout
 *                      is not one this decoder has seen
 *   IDX_ERR_RANGE      a payload this recognised is truncated
 */
idx_status idx_venue_raydium_decode(const idx_transaction *tx,
                                    const idx_instruction *ix, idx_venue venue,
                                    idx_swap *out, idx_error *err);

#endif /* IDX_VENUE_RAYDIUM_H */
