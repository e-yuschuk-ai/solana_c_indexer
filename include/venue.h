/*
 * Trading venues and the swap they produce (ROADMAP.md milestone M6).
 *
 * D5 makes swaps the entity the whole price side of the design rests on, and
 * it fixes how they are read: only from what the block carried. A venue
 * decoder therefore says what one instruction — or the event that instruction
 * emitted — states outright, and nothing more. Whatever it leaves unset is for
 * the normalization step to resolve against the balance deltas of the accounts
 * named here.
 *
 * Two shapes of venue exist on mainnet, and the difference decides how each is
 * read:
 *
 *   Anchor programs that emit a CPI event (`pump.fun`, PumpSwap, Jupiter)
 *   self-invoke with a fixed 8-byte marker and a payload holding the amounts
 *   the program itself computed. That payload is the authority: it survives
 *   the account-order changes a program upgrade brings, and it states what was
 *   traded rather than what was requested. These venues are read from their
 *   events.
 *
 *   Programs without events (Raydium's AMM v4) state only what the caller
 *   asked for — an input amount and a limit — so the amounts must come from
 *   the vault deltas, and the decoder's job is to name the vaults.
 *
 * Both kinds land in the same `idx_swap`, distinguished by `from_event`.
 *
 * An aggregator is not a venue in this sense. Jupiter routes through the pools
 * above, so a route both is not a pool and contains the pool swaps that are:
 * counting it as a swap of its own would double every routed trade, and its
 * `SwapEvent.amm` is not even reliably a pool — for a PumpSwap leg it carries
 * the PumpSwap *program* id. Jupiter is decoded for attribution — this trade
 * was routed — and never as a pool swap. `idx_venue_is_pool` draws that line.
 */
#ifndef IDX_VENUE_H
#define IDX_VENUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "error.h"
#include "types.h"

typedef enum {
    IDX_VENUE_NONE = 0,
    IDX_VENUE_PUMP_CURVE,     /* pump.fun bonding curve */
    IDX_VENUE_PUMP_AMM,       /* PumpSwap, where a curve graduates to */
    IDX_VENUE_RAYDIUM_AMM_V4, /* the constant-product pools */
    IDX_VENUE_RAYDIUM_CLMM,   /* concentrated liquidity */
    IDX_VENUE_RAYDIUM_CPMM,   /* the newer constant-product program */
    IDX_VENUE_JUPITER         /* aggregator, not a pool */
} idx_venue;

/* Lowercase name ("pump_curve", "raydium_amm_v4"), for logs. Never NULL. */
const char *idx_venue_name(idx_venue venue);

/* The venue a program id belongs to, or IDX_VENUE_NONE. */
idx_venue idx_venue_of_program(const idx_pubkey *program_id);

/* False for an aggregator, whose rows describe a route rather than a pool and
 * whose legs are recorded from the venues it routed through. */
bool idx_venue_is_pool(idx_venue venue);

/* Program ids, exposed so a caller can match without going through the
 * lookup. The base58 form of each is in the definition's comment, and a test
 * asserts the two agree. */
extern const idx_pubkey IDX_PROGRAM_PUMP_CURVE;
extern const idx_pubkey IDX_PROGRAM_PUMP_AMM;
extern const idx_pubkey IDX_PROGRAM_RAYDIUM_AMM_V4;
extern const idx_pubkey IDX_PROGRAM_RAYDIUM_CLMM;
extern const idx_pubkey IDX_PROGRAM_RAYDIUM_CPMM;
extern const idx_pubkey IDX_PROGRAM_JUPITER;

/*
 * Wrapped SOL. The pump.fun curve trades native lamports rather than a token,
 * and reporting that side as the wrapped mint is what lets one quote set cover
 * both — D5 already names "SOL/WSOL" as a single quote. Nothing is wrapped by
 * saying so; it is the identity the row carries.
 */
extern const idx_pubkey IDX_MINT_WSOL;

/*
 * The 8-byte marker an Anchor program self-invokes with to emit an event. The
 * payload that follows is the event's own discriminator and then its fields.
 */
extern const uint8_t IDX_ANCHOR_EVENT_DISCRIMINATOR[8];

/*
 * One swap, as the venue states it. Everything is optional but `venue`,
 * because what a venue names varies: an event carries amounts and no mints, an
 * instruction carries accounts and a requested amount. The normalization step
 * fills the rest from `meta`, which is why the token accounts are here at all.
 *
 * Amounts are from the trader's side: `input` left the trader, `output`
 * reached them. For a venue whose event speaks in base and quote, the event
 * kind gives the direction and this is the translation of it.
 */
typedef struct {
    idx_venue venue;
    bool from_event; /* the program's own CPI event, not the instruction */

    idx_pubkey pool; /* the pool account, not the program */
    bool has_pool;
    idx_pubkey user; /* the trader */
    bool has_user;

    idx_pubkey input_mint;
    bool has_input_mint;
    idx_pubkey output_mint;
    bool has_output_mint;

    /* The trader's token accounts, which name the mints when the venue does
     * not: the block's token balances say what each one holds. */
    idx_pubkey input_account;
    bool has_input_account;
    idx_pubkey output_account;
    bool has_output_account;

    uint64_t input_amount;
    uint64_t output_amount;
    bool has_input_amount;
    bool has_output_amount;

    /* The pool's own token accounts, for a venue that states no amounts: what
     * moved through them is the swap. */
    idx_pubkey pool_account_a;
    idx_pubkey pool_account_b;
    bool has_pool_accounts;
} idx_swap;

/*
 * Decodes `ix` as a swap of whatever venue its program belongs to.
 *
 *   IDX_OK             `out` is populated
 *   IDX_ERR_NOT_FOUND  the program is not a venue this knows, or the
 *                      instruction is one of its non-trading ones — the
 *                      overwhelmingly common answer, and not a failure
 *   IDX_ERR_RANGE      a payload this decoder recognised is truncated
 *   IDX_ERR_PARSE      the instruction names fewer accounts than the variant
 *                      operates on
 *
 * A venue that emits events yields swaps from those events only, so a `buy`
 * instruction and the `TradeEvent` it emitted do not both produce a row.
 */
idx_status idx_swap_decode(const idx_transaction *tx, const idx_instruction *ix,
                           idx_swap *out, idx_error *err);

/*
 * True when `data` starts with the Anchor CPI event marker, in which case
 * `*payload` is set to everything after it: the event discriminator and its
 * fields. The venue modules use this to tell an event from an instruction.
 */
bool idx_anchor_event_payload(idx_slice data, idx_slice *payload);

/* True when `payload` — what the call above produced — carries the event whose
 * 8-byte discriminator is `discriminator`, with `*fields` set past it. */
bool idx_anchor_event_is(idx_slice payload, const uint8_t discriminator[8],
                         idx_slice *fields);

#endif /* IDX_VENUE_H */
