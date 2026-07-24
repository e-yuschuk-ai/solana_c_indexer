/*
 * System program instruction decoding (ROADMAP.md milestone M5).
 *
 * `idx_block_decode` leaves instruction data as opaque bytes. This module
 * reads the bytes of an instruction whose program is the System program and
 * returns the variant and its fields.
 *
 * The wire format is bincode with fixed-width integers, as the runtime
 * deserializes it: a little-endian `u32` discriminant, then the fields in
 * declaration order, little-endian, unpadded. A `String` — the seed of the
 * `WithSeed` variants — is a little-endian `u64` length followed by its bytes.
 *
 * Accounts are exposed as pointers into the transaction's resolved account
 * list, so a decoded instruction borrows both `tx` and the arena the block was
 * decoded into. Seeds borrow the instruction data for the same lifetime.
 */
#ifndef IDX_SYSTEM_PROGRAM_H
#define IDX_SYSTEM_PROGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "bytes.h"
#include "error.h"
#include "types.h"

/* Discriminants, in the order the runtime's `SystemInstruction` declares
 * them. The numbering is part of the chain format, so it is written out. */
typedef enum {
    IDX_SYSTEM_IX_CREATE_ACCOUNT = 0,
    IDX_SYSTEM_IX_ASSIGN = 1,
    IDX_SYSTEM_IX_TRANSFER = 2,
    IDX_SYSTEM_IX_CREATE_ACCOUNT_WITH_SEED = 3,
    IDX_SYSTEM_IX_ADVANCE_NONCE_ACCOUNT = 4,
    IDX_SYSTEM_IX_WITHDRAW_NONCE_ACCOUNT = 5,
    IDX_SYSTEM_IX_INITIALIZE_NONCE_ACCOUNT = 6,
    IDX_SYSTEM_IX_AUTHORIZE_NONCE_ACCOUNT = 7,
    IDX_SYSTEM_IX_ALLOCATE = 8,
    IDX_SYSTEM_IX_ALLOCATE_WITH_SEED = 9,
    IDX_SYSTEM_IX_ASSIGN_WITH_SEED = 10,
    IDX_SYSTEM_IX_TRANSFER_WITH_SEED = 11,
    IDX_SYSTEM_IX_UPGRADE_NONCE_ACCOUNT = 12
} idx_system_ix_kind;

/* Variant name as the runtime spells it ("Transfer"), for log and error
 * messages. Never NULL. */
const char *idx_system_ix_kind_name(idx_system_ix_kind kind);

/*
 * A decoded System instruction. `kind` selects the union member: each variant
 * has its own, named after it, except the nonce variants that share a shape.
 *
 * A pubkey that the instruction carries in its *data* is held by value; one
 * that it names through an *account* is a pointer into `tx->accounts`. The
 * distinction matters: only the second kind is an account the transaction
 * touched, and therefore the only one with a balance delta in `meta`.
 */
typedef struct {
    idx_system_ix_kind kind;
    union {
        struct {
            const idx_pubkey *funder;
            const idx_pubkey *account;
            uint64_t lamports;
            uint64_t space;
            idx_pubkey owner;
        } create_account;

        struct {
            const idx_pubkey *account;
            idx_pubkey owner;
        } assign;

        struct {
            const idx_pubkey *from;
            const idx_pubkey *to;
            uint64_t lamports;
        } transfer;

        struct {
            const idx_pubkey *funder;
            const idx_pubkey *account;
            idx_pubkey base;
            idx_slice seed;
            uint64_t lamports;
            uint64_t space;
            idx_pubkey owner;
        } create_account_with_seed;

        /* AdvanceNonceAccount and UpgradeNonceAccount. `authority` is NULL for
         * the upgrade, which takes the nonce account alone. */
        struct {
            const idx_pubkey *nonce;
            const idx_pubkey *authority;
        } nonce;

        struct {
            const idx_pubkey *nonce;
            const idx_pubkey *to;
            const idx_pubkey *authority;
            uint64_t lamports;
        } withdraw_nonce;

        /* InitializeNonceAccount and AuthorizeNonceAccount. The initialize
         * form has no signing authority account, so `authority` is NULL
         * there; `new_authority` is the one the data carries in both. */
        struct {
            const idx_pubkey *nonce;
            const idx_pubkey *authority;
            idx_pubkey new_authority;
        } set_nonce_authority;

        struct {
            const idx_pubkey *account;
            uint64_t space;
        } allocate;

        struct {
            const idx_pubkey *account;
            const idx_pubkey *base_account; /* the signer, not `base` */
            idx_pubkey base;
            idx_slice seed;
            uint64_t space;
            idx_pubkey owner;
        } allocate_with_seed;

        struct {
            const idx_pubkey *account;
            const idx_pubkey *base_account;
            idx_pubkey base;
            idx_slice seed;
            idx_pubkey owner;
        } assign_with_seed;

        struct {
            const idx_pubkey *from;
            const idx_pubkey *base_account;
            const idx_pubkey *to;
            uint64_t lamports;
            idx_slice from_seed;
            idx_pubkey from_owner;
        } transfer_with_seed;
    };
} idx_system_instruction;

/*
 * Decodes `ix`, which the caller has already established belongs to the
 * System program (`idx_instruction_program_id` against `IDX_PROGRAM_SYSTEM`).
 * Works for a top-level instruction and for an inner one alike, since both
 * index the same account list.
 *
 *   IDX_OK             `out` is populated
 *   IDX_ERR_NOT_FOUND  the discriminant is not one this decoder knows, which
 *                      is a program upgrade rather than bad data: skip it
 *   IDX_ERR_RANGE      the payload is truncated — a field, or a seed of the
 *                      stated length, runs past the end of the data
 *   IDX_ERR_PARSE      the instruction names fewer accounts than the variant
 *                      operates on
 *
 * Bytes left over after a variant's fields are ignored, because the runtime
 * ignores them too: rejecting an instruction the chain executed would drop a
 * real event.
 */
idx_status idx_system_instruction_decode(const idx_transaction *tx,
                                         const idx_instruction *ix,
                                         idx_system_instruction *out,
                                         idx_error *err);

#endif /* IDX_SYSTEM_PROGRAM_H */
