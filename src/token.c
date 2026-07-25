#include "token.h"

#include <stdlib.h>
#include <string.h>

void idx_token_registry_init(idx_token_registry *reg) {
    if (reg == NULL) {
        return;
    }
    idx_map_init(&reg->by_mint);
    reg->with_metadata = 0;
}

void idx_token_registry_free(idx_token_registry *reg) {
    if (reg == NULL) {
        return;
    }
    size_t cursor = 0;
    idx_slice key;
    void *value = NULL;
    while (idx_map_next(&reg->by_mint, &cursor, &key, &value)) {
        free(value);
    }
    idx_map_free(&reg->by_mint);
    reg->with_metadata = 0;
}

static idx_slice mint_key(const idx_pubkey *mint) {
    return idx_slice_make(mint->bytes, IDX_PUBKEY_LEN);
}

/* Finds the token for `mint`, or creates and inserts an empty one stamped with
 * `slot`, returning it through `out`. */
static idx_status find_or_create(idx_token_registry *reg,
                                const idx_pubkey *mint, idx_slot slot,
                                idx_token **out, idx_error *err) {
    void *value = NULL;
    if (idx_map_get(&reg->by_mint, mint_key(mint), &value)) {
        *out = (idx_token *)value;
        return IDX_OK;
    }
    idx_token *token = calloc(1, sizeof(*token));
    if (token == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "out of memory");
    }
    token->mint = *mint;
    token->first_seen_slot = slot;
    idx_status status = idx_map_put(&reg->by_mint, mint_key(mint), token, err);
    if (status != IDX_OK) {
        free(token);
        return status;
    }
    *out = token;
    return IDX_OK;
}

idx_status idx_token_registry_observe_balance(idx_token_registry *reg,
                                              const idx_pubkey *mint,
                                              uint8_t decimals, idx_slot slot,
                                              idx_error *err) {
    if (reg == NULL || mint == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "reg and mint must not be NULL");
    }
    idx_token *token = NULL;
    IDX_TRY(find_or_create(reg, mint, slot, &token, err));
    if (!token->has_decimals) {
        token->decimals = decimals;
        token->has_decimals = true;
    }
    return IDX_OK;
}

/* Copies a borrowed string into a fixed buffer, truncating and NUL-terminating.
 * An empty slice leaves the field unset. */
static void copy_string(char *dest, size_t dest_size, idx_slice src,
                        bool *has) {
    if (src.data == NULL || src.len == 0) {
        return;
    }
    size_t n = src.len < dest_size - 1 ? src.len : dest_size - 1;
    memcpy(dest, src.data, n);
    dest[n] = '\0';
    *has = true;
}

idx_status idx_token_registry_observe_metadata(idx_token_registry *reg,
                                               const idx_token_metadata *meta,
                                               idx_slot slot, idx_error *err) {
    if (reg == NULL || meta == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "reg and meta must not be NULL");
    }
    if (!meta->has_mint) {
        return IDX_OK; /* nothing to key on */
    }
    idx_token *token = NULL;
    IDX_TRY(find_or_create(reg, &meta->mint, slot, &token, err));
    if (token->has_metadata) {
        return IDX_OK; /* keep the first metadata seen */
    }

    if (meta->has_name) {
        copy_string(token->name, sizeof(token->name), meta->name,
                    &token->has_name);
    }
    if (meta->has_symbol) {
        copy_string(token->symbol, sizeof(token->symbol), meta->symbol,
                    &token->has_symbol);
    }
    if (meta->has_uri) {
        copy_string(token->uri, sizeof(token->uri), meta->uri, &token->has_uri);
    }
    /* Counted as metadata even if every string was empty: a metadata
     * instruction was still what named it, and this stops a later one from
     * overwriting with the same emptiness on every observation. */
    token->has_metadata = true;
    reg->with_metadata++;
    return IDX_OK;
}

const idx_token *idx_token_registry_get(const idx_token_registry *reg,
                                        const idx_pubkey *mint) {
    if (reg == NULL || mint == NULL) {
        return NULL;
    }
    void *value = NULL;
    if (idx_map_get(&reg->by_mint, mint_key(mint), &value)) {
        return (const idx_token *)value;
    }
    return NULL;
}

size_t idx_token_registry_count(const idx_token_registry *reg) {
    if (reg == NULL) {
        return 0;
    }
    return idx_map_count(&reg->by_mint);
}
