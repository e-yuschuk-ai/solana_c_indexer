/*
 * Decoded block model (ROADMAP.md milestone M5).
 *
 * The pipeline hands a block over still in its JSON form (idx_raw_block).
 * idx_block_decode turns that into typed structs: the block header, its
 * transactions, and each transaction's accounts and instructions. The input is
 * the `encoding: json` shape the transport already requests (decision D7), so
 * this is a structural decode — account keys, signatures and blockhashes are
 * base58 text to turn into bytes, and instruction data is base58 to turn into
 * an opaque byte string. Interpreting those instruction bytes per program is a
 * later item.
 *
 * Everything here is allocated from the arena passed to idx_block_decode and
 * borrows the JSON document the block was parsed from; both must outlive any
 * use of the result. This matches the pipeline's handler contract, where the
 * arena is reset the moment the handler returns.
 */
#ifndef IDX_BLOCK_H
#define IDX_BLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "bytes.h"
#include "error.h"
#include "json.h"
#include "types.h"

/* Transaction message version. Only legacy and v0 exist on mainnet today; a
 * higher version in a block is rejected rather than guessed at. */
typedef enum {
    IDX_TX_VERSION_LEGACY = 0,
    IDX_TX_VERSION_0 = 1
} idx_tx_version;

/* Lowercase name ("legacy", "v0"), for log and error messages. Never NULL. */
const char *idx_tx_version_name(idx_tx_version version);

/*
 * One account referenced by a transaction, after lookup-table resolution. The
 * flags are derived from the message header for the static keys and from the
 * loaded-address partition for the rest (a loaded address is never a signer).
 */
typedef struct {
    idx_pubkey pubkey;
    bool is_signer;
    bool is_writable;
    bool from_lookup_table; /* loaded via an address lookup table (v0 only) */
} idx_account;

/*
 * A compiled instruction. The indices address the transaction's resolved
 * account list (idx_transaction.accounts) and are validated in range at decode
 * time, so a consumer may use them without rechecking. `data` is the
 * base58-decoded instruction payload, borrowed from the arena.
 */
typedef struct {
    uint8_t program_id_index;
    const uint8_t *account_indices;
    size_t account_count;
    idx_slice data;
    uint16_t stack_height; /* 0 when the provider omits it */
} idx_instruction;

/* The inner instructions one top-level instruction expanded into (from meta). */
typedef struct {
    uint8_t index; /* the top-level instruction these belong to */
    const idx_instruction *instructions;
    size_t instruction_count;
} idx_inner_instructions;

typedef struct {
    idx_tx_version version;

    const idx_signature *signatures;
    size_t signature_count;

    /* Message header. */
    uint8_t num_required_signatures;
    uint8_t num_readonly_signed;
    uint8_t num_readonly_unsigned;
    idx_hash recent_blockhash;

    /*
     * The full account list an instruction index resolves against: the static
     * message keys first, then the loaded writable addresses, then the loaded
     * readonly ones. `static_account_count` marks where the static keys end.
     */
    const idx_account *accounts;
    size_t account_count;
    size_t static_account_count;

    const idx_instruction *instructions;
    size_t instruction_count;

    const idx_inner_instructions *inner_instructions;
    size_t inner_instruction_count;

    /*
     * Minimal metadata, carried because it is cheap and structural. The rest of
     * meta — balances, token balances, logs — is a later M5 item.
     */
    bool success; /* meta.err was null */
    uint64_t fee;
} idx_transaction;

typedef struct {
    idx_slot slot;
    idx_hash blockhash;
    idx_hash previous_blockhash;
    idx_slot parent_slot;

    uint64_t block_height; /* valid only when has_block_height */
    bool has_block_height;
    int64_t block_time; /* unix seconds; valid only when has_block_time */
    bool has_block_time;

    const idx_transaction *transactions;
    size_t transaction_count;
} idx_block;

/*
 * Decodes `block` — the `encoding: json` block object (blockhash, parentSlot,
 * transactions, ...) — for the given `slot`, allocating from `arena`. The slot
 * is passed separately because the block object does not carry its own.
 *
 *   IDX_OK             `out` is fully populated
 *   IDX_ERR_NOT_FOUND  a required field is absent
 *   IDX_ERR_PARSE      a field has the wrong type, an account index is out of
 *                      range, or a transaction is a version this decoder does
 *                      not support
 *   IDX_ERR_NO_MEMORY  the arena could not grow
 *
 * Assumes transactionDetails was "full"; a block fetched with less detail does
 * not carry the message and meta this reads.
 */
idx_status idx_block_decode(idx_json_val block, idx_slot slot, idx_arena *arena,
                            idx_block *out, idx_error *err);

/* The program a compiled instruction invokes. The index is in range by
 * construction, so this never fails. */
const idx_pubkey *idx_instruction_program_id(const idx_transaction *tx,
                                             const idx_instruction *ix);

#endif /* IDX_BLOCK_H */
