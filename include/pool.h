/*
 * Pool registry (ROADMAP.md milestone M6, decision D5).
 *
 * A pool is a dimension, not an event: the storage tiers hold one row per pool
 * address, and the price series (bars) is keyed by it. D5 fixes how that row is
 * built — from what the block stream carried and nothing fetched — and in which
 * order:
 *
 *   Structure is learned from the first swap observed. A swap names the pool,
 *   its two mints and their decimals (swap.h resolved them from meta), which is
 *   the whole of what a pool dimension needs. A pool the indexer never sees
 *   trade is a pool with no price series, so it is not a row at all.
 *
 *   A creation only enriches a record that already exists. The creation
 *   instruction adds what a swap cannot state — who created the pool — but it
 *   never creates the record: a creation seen for a pool that has not traded is
 *   dropped, because a pool with no swaps has nothing for the terminal to show.
 *   In practice a pool is created and first traded in the same transaction, so
 *   the swap registers it and the creation enriches it in one pass.
 *
 * The registry accumulates across blocks — a dimension outlives the block that
 * revealed it — so it owns its records rather than borrowing the per-block
 * arena. It is what M7 will back with the `pools` table; until then it lives in
 * the process. Not thread-safe: it belongs to the processing thread (D6).
 */
#ifndef IDX_POOL_H
#define IDX_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "map.h"
#include "swap.h"
#include "types.h"
#include "venue.h"

/*
 * One pool. The two mints are an unordered pair — a swap's input and output
 * depend on its direction, so a mint is matched to a slot by identity, not by
 * position, and either slot may fill before the other. Decimals ride with each
 * mint, filled from the first swap that carried them.
 */
typedef struct {
    idx_pubkey address;
    idx_venue venue;

    idx_pubkey mint_a;
    bool has_mint_a;
    uint8_t decimals_a;
    bool has_decimals_a;

    idx_pubkey mint_b;
    bool has_mint_b;
    uint8_t decimals_b;
    bool has_decimals_b;

    idx_slot first_seen_slot; /* the slot of the first swap that revealed it */
    uint64_t swap_count;      /* swaps observed against it, for observability */

    /* Creation enrichment (D5): set only when a creation was observed. */
    bool has_creation;
    idx_slot creation_slot;
    idx_pubkey creator;
    bool has_creator;
} idx_pool;

typedef struct {
    idx_map by_address; /* pubkey -> idx_pool* (heap-owned) */
    uint64_t enriched;  /* pools a creation added to */
    uint64_t creations_unmatched; /* creations for pools not (yet) traded */
} idx_pool_registry;

void idx_pool_registry_init(idx_pool_registry *reg);
void idx_pool_registry_free(idx_pool_registry *reg);

/*
 * Registers or enriches the pool a swap row names, at `slot`. A row with no pool
 * (an unresolved swap, or an aggregated route, which is not a pool — D8) is
 * ignored. The first row for an address creates the record; later rows fill any
 * mint or decimals still unknown and count the swap.
 *
 *   IDX_OK             observed (including the ignored-row case)
 *   IDX_ERR_NO_MEMORY  the registry could not grow
 */
idx_status idx_pool_registry_observe_swap(idx_pool_registry *reg,
                                          const idx_swap_row *row, idx_slot slot,
                                          idx_error *err);

/*
 * Enriches the pool a creation names with its creator, at `slot`. A creation for
 * a pool not in the registry is dropped rather than inserted (D5) and counted in
 * `creations_unmatched`; a pool already enriched keeps its first creation. This
 * never allocates and never fails on a well-formed registry, but takes an error
 * for signature symmetry.
 */
idx_status idx_pool_registry_observe_creation(idx_pool_registry *reg,
                                              const idx_pool_creation *creation,
                                              idx_slot slot, idx_error *err);

/* The record for `address`, or NULL. Borrows the registry's storage. */
const idx_pool *idx_pool_registry_get(const idx_pool_registry *reg,
                                      const idx_pubkey *address);

/* Distinct pools registered. */
size_t idx_pool_registry_count(const idx_pool_registry *reg);

#endif /* IDX_POOL_H */
