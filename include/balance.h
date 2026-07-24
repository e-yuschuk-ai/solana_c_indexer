/*
 * Balance state extraction (ROADMAP.md milestone M6).
 *
 * Decision D5 keeps balances as *state*, not as a log: the storage tiers hold
 * one current value per account, versioned by slot, and a consumer that wants
 * a balance over time reconstructs it from transfers and swaps. What this
 * module produces is therefore one observation per account per transaction,
 * which M7 writes as an upsert.
 *
 * `meta.preBalances` and `meta.postBalances` cover every account the
 * transaction touched, in the order of the resolved account list, so SOL
 * balances come out complete for the accounts observed and cost no decoding
 * beyond a subtraction.
 *
 * Only accounts whose balance actually moved are emitted. The ones that never
 * move are exactly the ones that appear in every transaction — program ids,
 * sysvars, the accounts a swap reads without touching — so emitting them would
 * write the hottest keys in the system over and over to say nothing had
 * happened. An account whose balance a transaction did not change has no
 * activity to show, and the transaction that does change it records it.
 *
 * Failed transactions are extracted too. Their fee was still charged, so the
 * post balance of the payer is a real observation; what was rolled back never
 * reached the post balances in the first place.
 *
 * Token balances follow the same shape but not the same wire form:
 * `meta.pre/postTokenBalances` are sparse and independent lists, so an
 * observation is the join of the two on the token account, and an account that
 * appears in only one of them is a creation or a close rather than an error.
 */
#ifndef IDX_BALANCE_H
#define IDX_BALANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "block.h"
#include "error.h"
#include "types.h"

/*
 * One account's lamport balance after a transaction, with the movement that
 * produced it. `delta` is `lamports` minus the balance before, and is never
 * zero: an observation that changed nothing is not emitted.
 */
typedef struct {
    idx_pubkey account;
    uint64_t lamports; /* after the transaction */
    int64_t delta;     /* negative when the account paid out */
} idx_sol_balance;

/*
 * Extracts the SOL balance changes of `tx` into an array allocated from
 * `arena`, which borrows nothing from the transaction and so outlives it as
 * long as the arena does.
 *
 * A transaction whose block was fetched without metadata carries no balances;
 * that is not an error, it yields no rows. Neither does a transaction that
 * moved nothing, which is possible when a fee is paid by an account that is
 * not in the list — `*out` is then NULL and `*out_count` zero.
 *
 *   IDX_OK             `out` and `out_count` are set
 *   IDX_ERR_NO_MEMORY  the arena could not grow
 *   IDX_ERR_RANGE      a balance moved by more lamports than an int64 holds,
 *                      which no supply allows and no real block carries
 */
idx_status idx_sol_balance_extract(const idx_transaction *tx, idx_arena *arena,
                                   const idx_sol_balance **out,
                                   size_t *out_count, idx_error *err);

/*
 * One token account's balance after a transaction. This is the entity D5 calls
 * `token_balances`, as opposed to `idx_token_balance`, which is one raw
 * `meta.pre/postTokenBalances` entry — this struct is the join of the two sides
 * for one account, and it carries the account key the wire entries only point
 * at by index.
 *
 * The movement is `amount` against `previous` rather than a signed delta, which
 * is where this parts ways with `idx_sol_balance`. Lamport balances are bounded
 * by a supply two orders of magnitude below what an int64 holds; a raw token
 * amount is bounded only by the uint64 the mint's supply lives in, so a single
 * transfer of a mint with more than `INT64_MAX` raw units in circulation does
 * not fit in a signed difference. Those mints exist, so the subtraction is left
 * to a consumer that knows what width it needs.
 *
 * `mint` and `decimals` come with every observation; `owner` is present in
 * recent history but absent from older blocks, hence the flag.
 */
typedef struct {
    idx_pubkey account; /* the token account, not its owner */
    idx_pubkey mint;
    idx_pubkey owner;  /* the wallet holding it; valid only when has_owner */
    uint64_t amount;   /* raw units after the transaction */
    uint64_t previous; /* raw units before; 0 when !existed_before */
    uint8_t decimals;
    bool has_owner;
    bool existed_before; /* the account held a balance of this mint before */
    bool closed;         /* it held none after — a close, in practice */
} idx_token_balance_state;

/*
 * Extracts the token balance changes of `tx` into an array allocated from
 * `arena`, which borrows nothing from the transaction.
 *
 * The two wire lists are joined on the token account *and* its mint. Keying on
 * the account alone would be enough for every transaction a real block carries,
 * since a token account's mint never changes; keying on both means the exotic
 * close-and-recreate-as-another-mint reads as what it is — one account emptied,
 * one account funded — instead of a subtraction across two different tokens.
 *
 * As with SOL, only accounts whose balance moved are emitted, so a transaction
 * that reads a pool's vaults without trading against them costs no rows. An
 * account absent from the post list is emitted with `amount` zero and `closed`
 * set: it held tokens and no longer does, which is a movement like any other.
 * A newly funded account is the mirror image, with `existed_before` clear. An
 * account created empty or closed while already empty moved nothing, and the
 * same rule drops it — an empty token account has nothing for a terminal to
 * show, and its mint reaches the token registry through any account that does
 * hold it.
 *
 *   IDX_OK             `out` and `out_count` are set, possibly to NULL and 0
 *   IDX_ERR_NO_MEMORY  the arena could not grow
 *   IDX_ERR_PARSE      one side lists the same token account twice, which makes
 *                      the join ambiguous and no node produces
 */
idx_status idx_token_balance_extract(const idx_transaction *tx, idx_arena *arena,
                                     const idx_token_balance_state **out,
                                     size_t *out_count, idx_error *err);

#endif /* IDX_BALANCE_H */
