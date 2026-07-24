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
 */
#ifndef IDX_BALANCE_H
#define IDX_BALANCE_H

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

#endif /* IDX_BALANCE_H */
