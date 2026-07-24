/*
 * SPL Token-2022 instruction decoding (ROADMAP.md milestone M5).
 *
 * Token-2022 is SPL Token plus an extension surface. Discriminants 0 to 24 are
 * the shared base set, which `idx_token_instruction_decode` already reads;
 * this module decodes an instruction of either range and reports which one it
 * got, so a consumer that has recognised the program makes one call.
 *
 * Beyond the base set there are two shapes. A handful of instructions stand on
 * their own — a mint close authority, a permanent delegate — and are decoded
 * in full. The rest are *extension groups*: a group discriminant followed by a
 * second byte selecting an instruction within the extension, and then a
 * payload whose layout is the extension's own. Decision D5 bounds this
 * deliberately: the extension surface is larger than everything else in M5 put
 * together, so a group is identified — group, sub-discriminant and the
 * undecoded payload bytes — and per-extension payloads are decoded when a
 * consumer needs them, not up front.
 *
 * Not covered: the token metadata and token group interfaces, which
 * Token-2022 dispatches by an eight-byte discriminator when the first byte
 * matches no instruction here. They are how a mint's name, symbol and URI
 * reach the chain, so the item that builds the token dimension is what should
 * add them.
 */
#ifndef IDX_TOKEN_2022_H
#define IDX_TOKEN_2022_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "bytes.h"
#include "error.h"
#include "token_program.h"
#include "types.h"

/*
 * The instructions Token-2022 has beyond the base set, numbered as the program
 * numbers them. The `_EXTENSION` members are groups: their payload starts with
 * a sub-discriminant this decoder reads but does not interpret.
 */
typedef enum {
    IDX_TOKEN_2022_IX_INITIALIZE_MINT_CLOSE_AUTHORITY = 25,
    IDX_TOKEN_2022_IX_TRANSFER_FEE_EXTENSION = 26,
    IDX_TOKEN_2022_IX_CONFIDENTIAL_TRANSFER_EXTENSION = 27,
    IDX_TOKEN_2022_IX_DEFAULT_ACCOUNT_STATE_EXTENSION = 28,
    IDX_TOKEN_2022_IX_REALLOCATE = 29,
    IDX_TOKEN_2022_IX_MEMO_TRANSFER_EXTENSION = 30,
    IDX_TOKEN_2022_IX_CREATE_NATIVE_MINT = 31,
    IDX_TOKEN_2022_IX_INITIALIZE_NON_TRANSFERABLE_MINT = 32,
    IDX_TOKEN_2022_IX_INTEREST_BEARING_MINT_EXTENSION = 33,
    IDX_TOKEN_2022_IX_CPI_GUARD_EXTENSION = 34,
    IDX_TOKEN_2022_IX_INITIALIZE_PERMANENT_DELEGATE = 35,
    IDX_TOKEN_2022_IX_TRANSFER_HOOK_EXTENSION = 36,
    IDX_TOKEN_2022_IX_CONFIDENTIAL_TRANSFER_FEE_EXTENSION = 37,
    IDX_TOKEN_2022_IX_WITHDRAW_EXCESS_LAMPORTS = 38,
    IDX_TOKEN_2022_IX_METADATA_POINTER_EXTENSION = 39,
    IDX_TOKEN_2022_IX_GROUP_POINTER_EXTENSION = 40,
    IDX_TOKEN_2022_IX_GROUP_MEMBER_POINTER_EXTENSION = 41,
    IDX_TOKEN_2022_IX_CONFIDENTIAL_MINT_BURN_EXTENSION = 42,
    IDX_TOKEN_2022_IX_SCALED_UI_AMOUNT_EXTENSION = 43,
    IDX_TOKEN_2022_IX_PAUSABLE_EXTENSION = 44
} idx_token_2022_ix_kind;

/* Variant name as the program spells it ("TransferFeeExtension"), for log and
 * error messages. Never NULL. */
const char *idx_token_2022_ix_kind_name(idx_token_2022_ix_kind kind);

/* True for a group whose payload this decoder leaves undecoded. */
bool idx_token_2022_ix_is_extension(idx_token_2022_ix_kind kind);

/*
 * A decoded Token-2022 instruction.
 *
 * `is_base` says which half of the struct is live. When it is true the
 * instruction is one of the shared ones and `base` holds it, decoded exactly
 * as `idx_token_instruction_decode` would; `kind` and the union mean nothing.
 * When it is false `kind` selects the union member and `base` means nothing.
 */
typedef struct {
    bool is_base;
    idx_token_instruction base;

    idx_token_2022_ix_kind kind;
    union {
        /* InitializeMintCloseAuthority. No close authority disables closing
         * the mint rather than leaving it unset. */
        struct {
            const idx_pubkey *mint;
            idx_pubkey close_authority;
            bool has_close_authority;
        } initialize_mint_close_authority;

        struct {
            const idx_pubkey *mint;
            idx_pubkey delegate;
        } initialize_permanent_delegate;

        /* InitializeNonTransferableMint, and the mint of CreateNativeMint. */
        struct {
            const idx_pubkey *mint;
            const idx_pubkey *payer; /* NULL for the non-transferable mint */
        } mint_only;

        struct {
            const idx_pubkey *source;
            const idx_pubkey *destination;
            const idx_pubkey *authority;
        } withdraw_excess_lamports;

        /* Reallocate. The extension types are a bare sequence of little-endian
         * u16 values with no length prefix — read them with
         * idx_token_2022_extension_type_at. */
        struct {
            const idx_pubkey *account;
            const idx_pubkey *payer;
            const idx_pubkey *owner;
            idx_slice extension_types;
            size_t extension_type_count;
        } reallocate;

        /* Any of the `_EXTENSION` groups. The accounts are not resolved,
         * because which account means what depends on the sub-instruction,
         * and `payload` is everything after the sub-discriminant. */
        struct {
            uint8_t sub_discriminant;
            idx_slice payload;
        } extension;
    };
} idx_token_2022_instruction;

/*
 * The `index`-th extension type of a Reallocate, or 0 — the type the program
 * calls `Uninitialized`, which a real instruction never names — when `index`
 * is past the end.
 */
uint16_t idx_token_2022_extension_type_at(idx_slice extension_types,
                                          size_t index);

/*
 * Decodes `ix`, which the caller has established belongs to Token-2022.
 *
 *   IDX_OK             `out` is populated, `is_base` says how
 *   IDX_ERR_NOT_FOUND  the discriminant is past the last one this decoder
 *                      knows, which is a program upgrade or one of the
 *                      eight-byte metadata interface discriminators: skip it
 *   IDX_ERR_RANGE      the payload is truncated — a field, or the
 *                      sub-discriminant of an extension group, runs past the
 *                      end of the data
 *   IDX_ERR_PARSE      the instruction names fewer accounts than the variant
 *                      operates on, an optional pubkey carries a tag that is
 *                      neither 0 nor 1, or a Reallocate names half an
 *                      extension type
 */
idx_status idx_token_2022_instruction_decode(const idx_transaction *tx,
                                             const idx_instruction *ix,
                                             idx_token_2022_instruction *out,
                                             idx_error *err);

#endif /* IDX_TOKEN_2022_H */
