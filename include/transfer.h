/*
 * Transfer extraction (ROADMAP.md milestone M6).
 *
 * Decision D5 keeps transfers as *events*: one row per instruction that moved
 * value, identified by where in the block it ran. Balances are the state those
 * events add up to, and this is the log a consumer replays to explain how a
 * balance got where it is.
 *
 * The rows come from decoded instructions, top-level and inner alike — most
 * token movement on Solana happens inside a CPI from a venue's program, so a
 * walk of the top level alone would miss the majority of it.
 *
 * What produces a row:
 *
 *   SOL    the System instructions that move lamports — Transfer,
 *          TransferWithSeed, CreateAccount, CreateAccountWithSeed and
 *          WithdrawNonceAccount. Funding a new account is a movement like any
 *          other: the rent that creates a token account leaves a wallet and
 *          has to be accounted for somewhere.
 *   TOKEN  Transfer and TransferChecked from either token program, and
 *          Token-2022's TransferCheckedWithFee, whose fee is carried on the
 *          row so a consumer can tell gross from net.
 *   MINT   MintTo and MintToChecked: tokens that did not exist before.
 *   BURN   Burn and BurnChecked: tokens that no longer exist.
 *
 * Mint and burn are transfers to and from the mint itself, which is how they
 * are modelled here — the alternative is a wallet whose balance grows with no
 * event to explain it, and a log that only sometimes reconciles is worse than
 * one row shape more.
 *
 * What does not produce a row, deliberately:
 *
 *   - The transaction fee. It is a lamport movement with no instruction behind
 *     it, and it is already on the transaction.
 *   - CloseAccount, which sends a token account's whole lamport balance to its
 *     destination — a WSOL unwrap included. The amount is not in the
 *     instruction, only in `meta`, and D5 assigns reasoning from balance
 *     deltas to the swap path. The SOL balance state records the movement; if
 *     the query side ever needs the event, this is where it goes.
 *   - Anything from a failed transaction. Its instructions were rolled back,
 *     so a transfer row would claim something happened that did not. The
 *     balance extractor does the opposite, and for the same reason: the fee
 *     really was charged, and the rest really was not.
 */
#ifndef IDX_TRANSFER_H
#define IDX_TRANSFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "block.h"
#include "error.h"
#include "types.h"

typedef enum {
    IDX_TRANSFER_SOL = 0, /* lamports, moved by a System instruction */
    IDX_TRANSFER_TOKEN,   /* an SPL Token or Token-2022 transfer */
    IDX_TRANSFER_MINT,    /* tokens minted into an account */
    IDX_TRANSFER_BURN     /* tokens burned out of one */
} idx_transfer_kind;

/* Lowercase name ("sol", "token", "mint", "burn"), for log and error messages.
 * Never NULL. */
const char *idx_transfer_kind_name(idx_transfer_kind kind);

/*
 * One transfer.
 *
 * `source` and `destination` are always set. For a mint the source is the mint
 * account the tokens came from, and for a burn the destination is the mint they
 * went back to, so every row reads as a movement from one key to another and
 * `kind` says what the ends are.
 *
 * The rest of D5's instruction path — the slot and the transaction index — is
 * the caller's: it is walking a block and holds both, and repeating them on
 * every row of every transaction would cost more than it explains.
 *
 * The fields the instruction does not name are filled from the token balances
 * in `meta` for the same accounts, which is what makes an unchecked `Transfer`
 * usable at all: it names neither the mint nor the scale, and the block says
 * both. `source_owner` and `destination_owner` are the wallets behind the
 * token accounts, resolved here rather than left to a join, because a token
 * account's owner can change and the state tier only ever holds the latest
 * one — an old transfer joined against it would be attributed to whoever holds
 * the account now.
 */
typedef struct {
    idx_transfer_kind kind;

    uint16_t instruction_index; /* the top-level instruction */
    uint16_t inner_index;       /* position within its inner instructions */
    bool inner;                 /* false when this is the top-level one */

    idx_pubkey source;
    idx_pubkey destination;
    idx_pubkey authority; /* the signer that moved it; when has_authority */
    idx_pubkey mint;      /* never set for a SOL transfer; when has_mint */
    idx_pubkey source_owner;      /* when has_source_owner */
    idx_pubkey destination_owner; /* when has_destination_owner */

    uint64_t amount; /* lamports, or raw token units */
    uint64_t fee;    /* withheld by a Token-2022 transfer fee; 0 otherwise */
    uint8_t decimals; /* the mint's scale; when has_decimals */

    bool has_authority;
    bool has_mint;
    bool has_decimals;
    bool has_source_owner;
    bool has_destination_owner;
} idx_transfer;

/*
 * Extracts the transfers of `tx` into an array allocated from `arena`, in the
 * order they executed: each top-level instruction followed by the inner ones it
 * expanded into.
 *
 * A transfer of nothing is not emitted. A zero-amount instruction moves no
 * value, and the extraction of balances draws the same line.
 *
 * A failed transaction yields no rows, and neither does one whose block was
 * fetched without the metadata that says whether it failed.
 *
 *   IDX_OK             `out` and `out_count` are set, possibly to NULL and 0
 *   IDX_ERR_NO_MEMORY  the arena could not grow
 *   IDX_ERR_RANGE      an instruction of a program this reads is truncated
 *   IDX_ERR_PARSE      an instruction of a program this reads names fewer
 *                      accounts than its variant operates on
 *
 * The last two mean the decoder disagrees with a transaction the chain ran to
 * completion, which is a bug here rather than bad data, so they are reported
 * instead of skipped. An instruction whose discriminant no decoder knows is a
 * program upgrade and is skipped, since a program this indexer has never heard
 * of is the normal case.
 */
idx_status idx_transfer_extract(const idx_transaction *tx, idx_arena *arena,
                                const idx_transfer **out, size_t *out_count,
                                idx_error *err);

#endif /* IDX_TRANSFER_H */
