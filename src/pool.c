#include "pool.h"

#include <stdlib.h>
#include <string.h>

void idx_pool_registry_init(idx_pool_registry *reg) {
    if (reg == NULL) {
        return;
    }
    idx_map_init(&reg->by_address);
    reg->enriched = 0;
    reg->creations_unmatched = 0;
}

void idx_pool_registry_free(idx_pool_registry *reg) {
    if (reg == NULL) {
        return;
    }
    /* The map copies keys but only stores the value pointer, so the records are
     * this module's to free. */
    size_t cursor = 0;
    idx_slice key;
    void *value = NULL;
    while (idx_map_next(&reg->by_address, &cursor, &key, &value)) {
        free(value);
    }
    idx_map_free(&reg->by_address);
    reg->enriched = 0;
    reg->creations_unmatched = 0;
}

static idx_slice address_key(const idx_pubkey *address) {
    return idx_slice_make(address->bytes, IDX_PUBKEY_LEN);
}

static idx_pool *find_pool(const idx_pool_registry *reg,
                          const idx_pubkey *address) {
    void *value = NULL;
    if (idx_map_get(&reg->by_address, address_key(address), &value)) {
        return (idx_pool *)value;
    }
    return NULL;
}

/*
 * Records one side's mint against the pool's two slots. A mint is matched by
 * identity, so a buy and a sell of the same pool land on the same slots whatever
 * order their sides arrive in; the first swap to carry a side's decimals fills
 * them, and a side seen without them earlier is completed later.
 */
static void remember_mint(idx_pool *pool, bool has_mint, const idx_pubkey *mint,
                         bool has_decimals, uint8_t decimals) {
    if (!has_mint) {
        return;
    }
    if (pool->has_mint_a && idx_pubkey_equal(&pool->mint_a, mint)) {
        if (!pool->has_decimals_a && has_decimals) {
            pool->decimals_a = decimals;
            pool->has_decimals_a = true;
        }
        return;
    }
    if (pool->has_mint_b && idx_pubkey_equal(&pool->mint_b, mint)) {
        if (!pool->has_decimals_b && has_decimals) {
            pool->decimals_b = decimals;
            pool->has_decimals_b = true;
        }
        return;
    }
    if (!pool->has_mint_a) {
        pool->mint_a = *mint;
        pool->has_mint_a = true;
        pool->decimals_a = decimals;
        pool->has_decimals_a = has_decimals;
        return;
    }
    if (!pool->has_mint_b) {
        pool->mint_b = *mint;
        pool->has_mint_b = true;
        pool->decimals_b = decimals;
        pool->has_decimals_b = has_decimals;
        return;
    }
    /* Both slots are full and this is neither: a pool with three mints, which no
     * two-sided pool has. Leave the pair as first learned rather than churn it. */
}

idx_status idx_pool_registry_observe_swap(idx_pool_registry *reg,
                                          const idx_swap_row *row, idx_slot slot,
                                          idx_error *err) {
    if (reg == NULL || row == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "reg and row must not be NULL");
    }
    /* A route row is not a pool (D8), and an unresolved swap has nothing to key
     * on; either way there is no pool to register. */
    if (row->kind != IDX_SWAP_POOL || !row->has_pool) {
        return IDX_OK;
    }

    idx_pool *pool = find_pool(reg, &row->pool);
    if (pool == NULL) {
        pool = calloc(1, sizeof(*pool));
        if (pool == NULL) {
            return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "out of memory");
        }
        pool->address = row->pool;
        pool->venue = row->venue;
        pool->first_seen_slot = slot;
        idx_status status =
            idx_map_put(&reg->by_address, address_key(&row->pool), pool, err);
        if (status != IDX_OK) {
            free(pool);
            return status;
        }
    }

    remember_mint(pool, row->has_input_mint, &row->input_mint,
                  row->has_input_decimals, row->input_decimals);
    remember_mint(pool, row->has_output_mint, &row->output_mint,
                  row->has_output_decimals, row->output_decimals);
    pool->swap_count++;
    return IDX_OK;
}

idx_status idx_pool_registry_observe_creation(idx_pool_registry *reg,
                                              const idx_pool_creation *creation,
                                              idx_slot slot, idx_error *err) {
    if (reg == NULL || creation == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "reg and creation must not be NULL");
    }
    if (!creation->has_pool) {
        return IDX_OK;
    }

    idx_pool *pool = find_pool(reg, &creation->pool);
    if (pool == NULL) {
        /* A creation for a pool no swap has revealed. D5 keeps no record for it;
         * it is only counted, so the volume of dropped creations is visible. */
        reg->creations_unmatched++;
        return IDX_OK;
    }
    if (pool->has_creation) {
        return IDX_OK; /* keep the first creation seen */
    }

    pool->has_creation = true;
    pool->creation_slot = slot;
    if (creation->has_creator) {
        pool->creator = creation->creator;
        pool->has_creator = true;
    }
    reg->enriched++;
    return IDX_OK;
}

const idx_pool *idx_pool_registry_get(const idx_pool_registry *reg,
                                      const idx_pubkey *address) {
    if (reg == NULL || address == NULL) {
        return NULL;
    }
    return find_pool(reg, address);
}

size_t idx_pool_registry_count(const idx_pool_registry *reg) {
    if (reg == NULL) {
        return 0;
    }
    return idx_map_count(&reg->by_address);
}
