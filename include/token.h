/*
 * Token registry (ROADMAP.md milestone M6, decision D5).
 *
 * A token is a dimension keyed by mint address. D5 splits what is known about it
 * by how it arrives:
 *
 *   Address and decimals are free. Every token balance in meta carries the mint
 *   and its scale, so a token is registered the moment any account holding it
 *   moves, at no decoding cost.
 *
 *   Name, symbol and the metadata URI cost an observation. They live in a
 *   metadata instruction — pump.fun states them in its CreateEvent, the general
 *   case is the Metaplex Token Metadata program — so they are known only for
 *   tokens whose metadata instruction was seen, in practice those born after
 *   indexing started. The URI is stored unresolved: it points at JSON off-chain
 *   (Arweave, IPFS), and fetching it is a consumer's job, not the indexer's (D5).
 *
 * Like the pool registry, this accumulates across blocks and owns its records
 * rather than borrowing the per-block arena. It belongs to the processing
 * thread (D6) and is not thread-safe.
 */
#ifndef IDX_TOKEN_H
#define IDX_TOKEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "map.h"
#include "types.h"
#include "venue.h"

/* Metaplex caps name at 32 and symbol at 10; the buffers are generous so a
 * longer string from another source is truncated rather than overflowing, and
 * the URI holds the IPFS/Arweave links seen in practice. Stored NUL-terminated;
 * a longer value is truncated, which the length still reflects. */
#define IDX_TOKEN_NAME_MAX 64
#define IDX_TOKEN_SYMBOL_MAX 16
#define IDX_TOKEN_URI_MAX 200

typedef struct {
    idx_pubkey mint;

    uint8_t decimals;
    bool has_decimals;

    char name[IDX_TOKEN_NAME_MAX];
    bool has_name;
    char symbol[IDX_TOKEN_SYMBOL_MAX];
    bool has_symbol;
    char uri[IDX_TOKEN_URI_MAX];
    bool has_uri;

    idx_slot first_seen_slot;
    bool has_metadata; /* any of name/symbol/uri was observed */
} idx_token;

typedef struct {
    idx_map by_mint;       /* pubkey -> idx_token* (heap-owned) */
    uint64_t with_metadata; /* tokens a metadata instruction reached */
} idx_token_registry;

void idx_token_registry_init(idx_token_registry *reg);
void idx_token_registry_free(idx_token_registry *reg);

/*
 * Registers or completes a token from a balance observation: its mint and the
 * decimals every balance carries, at `slot`. The first observation creates the
 * record; a later one fills the decimals if an earlier source left them unset.
 *
 *   IDX_OK             registered
 *   IDX_ERR_NO_MEMORY  the registry could not grow
 */
idx_status idx_token_registry_observe_balance(idx_token_registry *reg,
                                              const idx_pubkey *mint,
                                              uint8_t decimals, idx_slot slot,
                                              idx_error *err);

/*
 * Enriches a token from a decoded metadata instruction, copying its name,
 * symbol and URI into the record (truncating any that overrun the buffers). A
 * metadata observation may create the record too — a token is a token whether or
 * not a balance has been seen yet. The first metadata seen wins; later ones do
 * not overwrite it.
 *
 *   IDX_OK             enriched (including when the metadata named no mint, a
 *                      no-op)
 *   IDX_ERR_NO_MEMORY  the registry could not grow
 */
idx_status idx_token_registry_observe_metadata(idx_token_registry *reg,
                                               const idx_token_metadata *meta,
                                               idx_slot slot, idx_error *err);

/* The record for `mint`, or NULL. Borrows the registry's storage. */
const idx_token *idx_token_registry_get(const idx_token_registry *reg,
                                        const idx_pubkey *mint);

/* Distinct tokens registered. */
size_t idx_token_registry_count(const idx_token_registry *reg);

#endif /* IDX_TOKEN_H */
