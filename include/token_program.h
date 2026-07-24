/*
 * SPL Token instruction decoding (ROADMAP.md milestone M5).
 *
 * The wire format is the one `TokenInstruction::pack` writes: a single-byte
 * discriminant, then the fields packed little-endian and unpadded. An optional
 * pubkey — the freeze authority of a mint, the new authority of a
 * `SetAuthority` — is a one-byte tag, 1 followed by the 32 bytes or 0 alone.
 * This is not bincode; the System program's format is a different one.
 *
 * Token-2022 shares this base instruction set, so the same decoder serves both
 * programs and `idx_token_program_matches` accepts either. What Token-2022 has
 * beyond it starts at discriminant 25 and belongs to `token_2022.h`, whose
 * decoder covers both ranges; here those decode as IDX_ERR_NOT_FOUND, the same
 * as any unknown variant.
 *
 * Accounts are pointers into the transaction's resolved account list, so a
 * decoded instruction borrows `tx` and the arena the block was decoded into.
 */
#ifndef IDX_TOKEN_PROGRAM_H
#define IDX_TOKEN_PROGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "bytes.h"
#include "error.h"
#include "types.h"

/* Discriminants, in the order `TokenInstruction` declares them. */
typedef enum {
    IDX_TOKEN_IX_INITIALIZE_MINT = 0,
    IDX_TOKEN_IX_INITIALIZE_ACCOUNT = 1,
    IDX_TOKEN_IX_INITIALIZE_MULTISIG = 2,
    IDX_TOKEN_IX_TRANSFER = 3,
    IDX_TOKEN_IX_APPROVE = 4,
    IDX_TOKEN_IX_REVOKE = 5,
    IDX_TOKEN_IX_SET_AUTHORITY = 6,
    IDX_TOKEN_IX_MINT_TO = 7,
    IDX_TOKEN_IX_BURN = 8,
    IDX_TOKEN_IX_CLOSE_ACCOUNT = 9,
    IDX_TOKEN_IX_FREEZE_ACCOUNT = 10,
    IDX_TOKEN_IX_THAW_ACCOUNT = 11,
    IDX_TOKEN_IX_TRANSFER_CHECKED = 12,
    IDX_TOKEN_IX_APPROVE_CHECKED = 13,
    IDX_TOKEN_IX_MINT_TO_CHECKED = 14,
    IDX_TOKEN_IX_BURN_CHECKED = 15,
    IDX_TOKEN_IX_INITIALIZE_ACCOUNT2 = 16,
    IDX_TOKEN_IX_SYNC_NATIVE = 17,
    IDX_TOKEN_IX_INITIALIZE_ACCOUNT3 = 18,
    IDX_TOKEN_IX_INITIALIZE_MULTISIG2 = 19,
    IDX_TOKEN_IX_INITIALIZE_MINT2 = 20,
    IDX_TOKEN_IX_GET_ACCOUNT_DATA_SIZE = 21,
    IDX_TOKEN_IX_INITIALIZE_IMMUTABLE_OWNER = 22,
    IDX_TOKEN_IX_AMOUNT_TO_UI_AMOUNT = 23,
    IDX_TOKEN_IX_UI_AMOUNT_TO_AMOUNT = 24
} idx_token_ix_kind;

/* Variant name as the program spells it ("TransferChecked"), for log and
 * error messages. Never NULL. */
const char *idx_token_ix_kind_name(idx_token_ix_kind kind);

/*
 * The `AuthorityType` of a SetAuthority. Token-2022 adds further values for
 * its extensions, so the decoded field is a raw byte and these name the ones
 * both programs share.
 */
typedef enum {
    IDX_TOKEN_AUTHORITY_MINT_TOKENS = 0,
    IDX_TOKEN_AUTHORITY_FREEZE_ACCOUNT = 1,
    IDX_TOKEN_AUTHORITY_ACCOUNT_OWNER = 2,
    IDX_TOKEN_AUTHORITY_CLOSE_ACCOUNT = 3
} idx_token_authority_type;

/* Name of an authority type ("AccountOwner"), or "unknown" for a value this
 * decoder does not name. Never NULL. */
const char *idx_token_authority_type_name(uint8_t type);

/*
 * A decoded SPL Token instruction. `kind` selects the union member; variants
 * that describe the same operation share one, so `Transfer` and
 * `TransferChecked` both populate `transfer` and are told apart by `kind` —
 * or, where it is what matters, by `has_decimals`.
 *
 * `has_decimals` marks the checked form of an operation: the caller was
 * required to state the mint's scale, and the program verified it. The
 * unchecked forms leave `decimals` zero, and for `transfer` and `approve`
 * they leave `mint` NULL, because those do not name the mint at all.
 *
 * An authority may be a multisig, in which case its signers follow as further
 * accounts. They are not resolved: nothing derived from an instruction depends
 * on which members of a multisig signed it.
 */
typedef struct {
    idx_token_ix_kind kind;
    union {
        /* InitializeMint and InitializeMint2. */
        struct {
            const idx_pubkey *mint;
            idx_pubkey mint_authority;
            idx_pubkey freeze_authority; /* valid when has_freeze_authority */
            bool has_freeze_authority;
            uint8_t decimals;
        } initialize_mint;

        /* InitializeAccount, InitializeAccount2 and InitializeAccount3. The
         * first names the owner as an account, the others carry it in the
         * data; `owner` holds it either way and `owner_account` is non-NULL
         * only for the first. */
        struct {
            const idx_pubkey *account;
            const idx_pubkey *mint;
            const idx_pubkey *owner_account;
            idx_pubkey owner;
        } initialize_account;

        /* InitializeMultisig and InitializeMultisig2. The signers are a
         * variable-length tail, given as indices into `tx->accounts`. */
        struct {
            const idx_pubkey *multisig;
            const uint8_t *signers;
            size_t signer_count;
            uint8_t required_signatures;
        } initialize_multisig;

        /* Transfer and TransferChecked. */
        struct {
            const idx_pubkey *source;
            const idx_pubkey *destination;
            const idx_pubkey *authority;
            const idx_pubkey *mint; /* NULL unless checked */
            uint64_t amount;
            uint8_t decimals;
            bool has_decimals;
        } transfer;

        /* Approve and ApproveChecked. */
        struct {
            const idx_pubkey *source;
            const idx_pubkey *delegate;
            const idx_pubkey *owner;
            const idx_pubkey *mint; /* NULL unless checked */
            uint64_t amount;
            uint8_t decimals;
            bool has_decimals;
        } approve;

        struct {
            const idx_pubkey *source;
            const idx_pubkey *owner;
        } revoke;

        struct {
            const idx_pubkey *account; /* a mint or a token account */
            const idx_pubkey *authority;
            idx_pubkey new_authority; /* valid when has_new_authority */
            bool has_new_authority;   /* false revokes the authority */
            uint8_t authority_type;   /* an idx_token_authority_type */
        } set_authority;

        /* MintTo and MintToChecked. */
        struct {
            const idx_pubkey *mint;
            const idx_pubkey *account;
            const idx_pubkey *authority;
            uint64_t amount;
            uint8_t decimals;
            bool has_decimals;
        } mint_to;

        /* Burn and BurnChecked. */
        struct {
            const idx_pubkey *account;
            const idx_pubkey *mint;
            const idx_pubkey *authority;
            uint64_t amount;
            uint8_t decimals;
            bool has_decimals;
        } burn;

        struct {
            const idx_pubkey *account;
            const idx_pubkey *destination;
            const idx_pubkey *owner;
        } close_account;

        /* FreezeAccount and ThawAccount. */
        struct {
            const idx_pubkey *account;
            const idx_pubkey *mint;
            const idx_pubkey *authority;
        } freeze_account;

        /* SyncNative and InitializeImmutableOwner. */
        struct {
            const idx_pubkey *account;
        } account_only;

        /* GetAccountDataSize and AmountToUiAmount. `amount` is zero for the
         * former, which carries none. */
        struct {
            const idx_pubkey *mint;
            uint64_t amount;
        } mint_query;

        /* UiAmountToAmount. The amount is decimal text, borrowed from the
         * instruction data and not NUL-terminated. */
        struct {
            const idx_pubkey *mint;
            idx_slice ui_amount;
        } ui_amount_to_amount;
    };
} idx_token_instruction;

/* True for the SPL Token program and for Token-2022, both of which this
 * decoder reads. */
bool idx_token_program_matches(const idx_pubkey *program_id);

/*
 * Decodes `ix`, which the caller has already established belongs to one of the
 * token programs. Works for a top-level instruction and for an inner one
 * alike, since both index the same account list.
 *
 *   IDX_OK             `out` is populated
 *   IDX_ERR_NOT_FOUND  the discriminant is not one this decoder knows — a
 *                      Token-2022 extension, or a program upgrade: skip it
 *   IDX_ERR_RANGE      the payload is truncated — a field runs past the end
 *                      of the data
 *   IDX_ERR_PARSE      the instruction names fewer accounts than the variant
 *                      operates on, or an optional pubkey carries a tag that
 *                      is neither 0 nor 1
 *
 * Bytes left over after a variant's fields are ignored, because the program
 * ignores them too: rejecting an instruction the chain executed would drop a
 * real event.
 */
idx_status idx_token_instruction_decode(const idx_transaction *tx,
                                        const idx_instruction *ix,
                                        idx_token_instruction *out,
                                        idx_error *err);

#endif /* IDX_TOKEN_PROGRAM_H */
