/*
 * Storage abstraction layer: dispatch and in-memory reference backends.
 *
 * The dispatch wrappers null-check the handle and forward to the vtable, so a
 * backend never repeats the check and a caller holding a NULL handle gets a
 * clear IDX_ERR_INVALID_ARG rather than a crash. The reference backends behind
 * idx_mem_*_store_open store rows in vectors and implement the slot-ranged
 * operations the interface promises; they are the executable spec a real
 * backend is checked against (see store.h).
 */
#include "store.h"

#include <stdlib.h>
#include <string.h>

#include "vec.h"

/* The eight entity kinds a write set carries, in a fixed order so the reference
 * stores can treat them as an array rather than eight named fields. */
enum {
    E_BLOCK = 0,
    E_SOL_BALANCE,
    E_TOKEN_BALANCE,
    E_TRANSFER,
    E_SWAP,
    E_POOL,
    E_TOKEN,
    E_BAR,
    E_COUNT
};

static const size_t g_elem_size[E_COUNT] = {
    sizeof(idx_store_block_row),         sizeof(idx_store_sol_balance_row),
    sizeof(idx_store_token_balance_row), sizeof(idx_store_transfer_row),
    sizeof(idx_store_swap_row),          sizeof(idx_pool),
    sizeof(idx_token),                   sizeof(idx_bar),
};

/* The slot each entity is deleted and read by (D5: every table carries one). */
typedef idx_slot (*slot_getter)(const void *elem);

static idx_slot block_slot(const void *e) {
    return ((const idx_store_block_row *)e)->slot;
}
static idx_slot sol_balance_slot(const void *e) {
    return ((const idx_store_sol_balance_row *)e)->ref.slot;
}
static idx_slot token_balance_slot(const void *e) {
    return ((const idx_store_token_balance_row *)e)->ref.slot;
}
static idx_slot transfer_slot(const void *e) {
    return ((const idx_store_transfer_row *)e)->ref.slot;
}
static idx_slot swap_slot(const void *e) {
    return ((const idx_store_swap_row *)e)->ref.slot;
}
static idx_slot pool_slot(const void *e) {
    return ((const idx_pool *)e)->first_seen_slot;
}
static idx_slot token_slot(const void *e) {
    return ((const idx_token *)e)->first_seen_slot;
}
static idx_slot bar_slot(const void *e) {
    /* close_seq is the latest swap in the bar, so its slot is the bar's max:
     * the bar holds a reorged swap exactly when this is at or above from_slot,
     * which mirrors idx_bar_registry_recompute_range. */
    return ((const idx_bar *)e)->close_seq.slot;
}

static const slot_getter g_slot_getter[E_COUNT] = {
    block_slot, sol_balance_slot, token_balance_slot, transfer_slot,
    swap_slot,  pool_slot,        token_slot,         bar_slot,
};

/* A write set's arrays as an indexable table, so the reference stores can loop
 * over the eight kinds instead of naming each. */
typedef struct {
    const void *data;
    size_t count;
} ws_array;

static void ws_arrays(const idx_store_write_set *set, ws_array out[E_COUNT]) {
    out[E_BLOCK] = (ws_array){set->blocks, set->block_count};
    out[E_SOL_BALANCE] = (ws_array){set->sol_balances, set->sol_balance_count};
    out[E_TOKEN_BALANCE] =
        (ws_array){set->token_balances, set->token_balance_count};
    out[E_TRANSFER] = (ws_array){set->transfers, set->transfer_count};
    out[E_SWAP] = (ws_array){set->swaps, set->swap_count};
    out[E_POOL] = (ws_array){set->pools, set->pool_count};
    out[E_TOKEN] = (ws_array){set->tokens, set->token_count};
    out[E_BAR] = (ws_array){set->bars, set->bar_count};
}

/* ------------------------------------------------------- write-set helpers -- */

void idx_store_write_set_init(idx_store_write_set *set) {
    if (set != NULL) {
        memset(set, 0, sizeof(*set));
    }
}

size_t idx_store_write_set_total(const idx_store_write_set *set) {
    if (set == NULL) {
        return 0;
    }
    ws_array arrays[E_COUNT];
    ws_arrays(set, arrays);
    size_t total = 0;
    for (int e = 0; e < E_COUNT; e++) {
        total += arrays[e].count;
    }
    return total;
}

size_t idx_store_counts_total(const idx_store_counts *counts) {
    if (counts == NULL) {
        return 0;
    }
    return counts->blocks + counts->sol_balances + counts->token_balances +
           counts->transfers + counts->swaps + counts->pools + counts->tokens +
           counts->bars;
}

/* --------------------------------------------------- confirmed dispatch ----- */

const char *idx_confirmed_store_name(const idx_confirmed_store *store) {
    if (store == NULL || store->vt == NULL || store->vt->name == NULL) {
        return "unset";
    }
    return store->vt->name(store->ctx);
}

idx_status idx_confirmed_store_write(idx_confirmed_store *store,
                                     const idx_store_write_set *set,
                                     idx_error *err) {
    if (store == NULL || store->vt == NULL || store->vt->write == NULL ||
        set == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "confirmed store not writable");
    }
    return store->vt->write(store->ctx, set, err);
}

idx_status idx_confirmed_store_reorg(idx_confirmed_store *store,
                                     idx_slot from_slot,
                                     const idx_store_write_set *replacement,
                                     idx_error *err) {
    if (store == NULL || store->vt == NULL || store->vt->reorg == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "confirmed store cannot reorg");
    }
    return store->vt->reorg(store->ctx, from_slot, replacement, err);
}

idx_status idx_confirmed_store_prune(idx_confirmed_store *store,
                                     idx_slot below_slot, idx_error *err) {
    if (store == NULL || store->vt == NULL || store->vt->prune == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "confirmed store cannot prune");
    }
    return store->vt->prune(store->ctx, below_slot, err);
}

idx_status idx_confirmed_store_read_range(idx_confirmed_store *store,
                                          idx_slot from_slot, idx_slot to_slot,
                                          idx_arena *arena,
                                          idx_store_write_set *out,
                                          idx_error *err) {
    if (store == NULL || store->vt == NULL || store->vt->read_range == NULL ||
        arena == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "confirmed store cannot be read");
    }
    if (from_slot > to_slot) {
        return IDX_FAIL(err, IDX_ERR_RANGE, "empty slot range: %llu > %llu",
                        (unsigned long long)from_slot,
                        (unsigned long long)to_slot);
    }
    return store->vt->read_range(store->ctx, from_slot, to_slot, arena, out, err);
}

void idx_confirmed_store_close(idx_confirmed_store *store) {
    if (store == NULL) {
        return;
    }
    if (store->vt != NULL && store->vt->close != NULL) {
        store->vt->close(store->ctx);
    }
    free(store);
}

/* --------------------------------------------------- finalized dispatch ----- */

const char *idx_finalized_store_name(const idx_finalized_store *store) {
    if (store == NULL || store->vt == NULL || store->vt->name == NULL) {
        return "unset";
    }
    return store->vt->name(store->ctx);
}

idx_status idx_finalized_store_append(idx_finalized_store *store,
                                      const idx_store_write_set *set,
                                      idx_error *err) {
    if (store == NULL || store->vt == NULL || store->vt->append == NULL ||
        set == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "finalized store not appendable");
    }
    return store->vt->append(store->ctx, set, err);
}

idx_status idx_finalized_store_flush(idx_finalized_store *store,
                                     idx_error *err) {
    if (store == NULL || store->vt == NULL || store->vt->flush == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "finalized store cannot flush");
    }
    return store->vt->flush(store->ctx, err);
}

void idx_finalized_store_close(idx_finalized_store *store) {
    if (store == NULL) {
        return;
    }
    if (store->vt != NULL && store->vt->close != NULL) {
        store->vt->close(store->ctx);
    }
    free(store);
}

/* ==================================================================== */
/* In-memory reference backends                                         */
/* ==================================================================== */

/* Both tiers hold the same eight row vectors; the finalized one adds the
 * batching bookkeeping the append-only tier needs. */
typedef struct {
    idx_vec rows[E_COUNT];
} mem_rows;

static void mem_rows_init(mem_rows *m) {
    for (int e = 0; e < E_COUNT; e++) {
        idx_vec_init(&m->rows[e], g_elem_size[e]);
    }
}

static void mem_rows_free(mem_rows *m) {
    for (int e = 0; e < E_COUNT; e++) {
        idx_vec_free(&m->rows[e]);
    }
}

/*
 * Appends every row of `set` into `m`, atomically: capacity for all eight
 * vectors is reserved up front, so a failure leaves the store untouched rather
 * than half written. Only IDX_ERR_NO_MEMORY can be reported.
 */
static idx_status mem_rows_append(mem_rows *m, const idx_store_write_set *set,
                                  idx_error *err) {
    if (set == NULL) {
        return IDX_OK;
    }
    ws_array arrays[E_COUNT];
    ws_arrays(set, arrays);
    for (int e = 0; e < E_COUNT; e++) {
        IDX_TRY(idx_vec_reserve(&m->rows[e], arrays[e].count, err));
    }
    for (int e = 0; e < E_COUNT; e++) {
        const char *base = arrays[e].data;
        for (size_t i = 0; i < arrays[e].count; i++) {
            /* Reserved above, so this push cannot fail. */
            (void)idx_vec_push(&m->rows[e], base + i * g_elem_size[e], err);
        }
    }
    return IDX_OK;
}

/* Keeps the rows for which `keep` is true, compacting each vector in place. */
typedef bool (*slot_pred)(idx_slot slot, idx_slot pivot);

static void mem_rows_retain(mem_rows *m, slot_pred keep, idx_slot pivot) {
    for (int e = 0; e < E_COUNT; e++) {
        idx_vec *v = &m->rows[e];
        size_t esz = v->elem_size;
        char *base = v->data;
        size_t w = 0;
        for (size_t r = 0; r < v->len; r++) {
            const void *elem = base + r * esz;
            if (keep(g_slot_getter[e](elem), pivot)) {
                if (w != r) {
                    memmove(base + w * esz, elem, esz);
                }
                w++;
            }
        }
        v->len = w;
    }
}

static bool keep_below(idx_slot slot, idx_slot pivot) { return slot < pivot; }
static bool keep_at_or_above(idx_slot slot, idx_slot pivot) {
    return slot >= pivot;
}

static void mem_rows_counts(const mem_rows *m, idx_store_counts *out) {
    out->blocks = m->rows[E_BLOCK].len;
    out->sol_balances = m->rows[E_SOL_BALANCE].len;
    out->token_balances = m->rows[E_TOKEN_BALANCE].len;
    out->transfers = m->rows[E_TRANSFER].len;
    out->swaps = m->rows[E_SWAP].len;
    out->pools = m->rows[E_POOL].len;
    out->tokens = m->rows[E_TOKEN].len;
    out->bars = m->rows[E_BAR].len;
}

/* Copies the rows of `v` whose slot is within [from, to] into a fresh arena
 * array, returning the array and its length (NULL and 0 when none match). */
static idx_status mem_collect_range(const idx_vec *v, slot_getter get,
                                    idx_slot from, idx_slot to, idx_arena *arena,
                                    void **out_data, size_t *out_count,
                                    idx_error *err) {
    size_t esz = v->elem_size;
    const char *base = v->data;
    size_t matched = 0;
    for (size_t r = 0; r < v->len; r++) {
        idx_slot s = get(base + r * esz);
        if (s >= from && s <= to) {
            matched++;
        }
    }
    if (matched == 0) {
        *out_data = NULL;
        *out_count = 0;
        return IDX_OK;
    }
    void *raw = NULL;
    IDX_TRY(idx_arena_calloc(arena, matched, esz, &raw, err));
    char *dst = raw;
    size_t w = 0;
    for (size_t r = 0; r < v->len; r++) {
        const void *elem = base + r * esz;
        idx_slot s = get(elem);
        if (s >= from && s <= to) {
            memcpy(dst + w * esz, elem, esz);
            w++;
        }
    }
    *out_data = raw;
    *out_count = matched;
    return IDX_OK;
}

/* ---------------------------------------------------- confirmed reference --- */

static const char *mem_confirmed_name(void *ctx) {
    (void)ctx;
    return "memory-confirmed";
}

static idx_status mem_confirmed_write(void *ctx, const idx_store_write_set *set,
                                      idx_error *err) {
    return mem_rows_append((mem_rows *)ctx, set, err);
}

static idx_status mem_confirmed_reorg(void *ctx, idx_slot from_slot,
                                      const idx_store_write_set *replacement,
                                      idx_error *err) {
    mem_rows *m = ctx;
    /* Reserve the replacement's capacity before deleting anything, so a
     * NO_MEMORY leaves the store as it was — the interface promises the reorg
     * is atomic (D4). */
    if (replacement != NULL) {
        ws_array arrays[E_COUNT];
        ws_arrays(replacement, arrays);
        for (int e = 0; e < E_COUNT; e++) {
            IDX_TRY(idx_vec_reserve(&m->rows[e], arrays[e].count, err));
        }
    }
    mem_rows_retain(m, keep_below, from_slot);
    return mem_rows_append(m, replacement, err);
}

static idx_status mem_confirmed_prune(void *ctx, idx_slot below_slot,
                                      idx_error *err) {
    (void)err;
    mem_rows_retain((mem_rows *)ctx, keep_at_or_above, below_slot);
    return IDX_OK;
}

static idx_status mem_confirmed_read_range(void *ctx, idx_slot from_slot,
                                           idx_slot to_slot, idx_arena *arena,
                                           idx_store_write_set *out,
                                           idx_error *err) {
    mem_rows *m = ctx;
    idx_store_write_set_init(out);

    void *data[E_COUNT] = {0};
    size_t count[E_COUNT] = {0};
    for (int e = 0; e < E_COUNT; e++) {
        IDX_TRY(mem_collect_range(&m->rows[e], g_slot_getter[e], from_slot,
                                  to_slot, arena, &data[e], &count[e], err));
    }
    out->blocks = data[E_BLOCK];
    out->block_count = count[E_BLOCK];
    out->sol_balances = data[E_SOL_BALANCE];
    out->sol_balance_count = count[E_SOL_BALANCE];
    out->token_balances = data[E_TOKEN_BALANCE];
    out->token_balance_count = count[E_TOKEN_BALANCE];
    out->transfers = data[E_TRANSFER];
    out->transfer_count = count[E_TRANSFER];
    out->swaps = data[E_SWAP];
    out->swap_count = count[E_SWAP];
    out->pools = data[E_POOL];
    out->pool_count = count[E_POOL];
    out->tokens = data[E_TOKEN];
    out->token_count = count[E_TOKEN];
    out->bars = data[E_BAR];
    out->bar_count = count[E_BAR];
    return IDX_OK;
}

static void mem_confirmed_close(void *ctx) {
    mem_rows *m = ctx;
    if (m != NULL) {
        mem_rows_free(m);
        free(m);
    }
}

static const idx_confirmed_store_vt g_mem_confirmed_vt = {
    .name = mem_confirmed_name,
    .write = mem_confirmed_write,
    .reorg = mem_confirmed_reorg,
    .prune = mem_confirmed_prune,
    .read_range = mem_confirmed_read_range,
    .close = mem_confirmed_close,
};

idx_status idx_mem_confirmed_store_open(idx_confirmed_store **out,
                                        idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out is null");
    }
    idx_confirmed_store *store = calloc(1, sizeof(*store));
    mem_rows *rows = calloc(1, sizeof(*rows));
    if (store == NULL || rows == NULL) {
        free(store);
        free(rows);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "confirmed store allocation");
    }
    mem_rows_init(rows);
    store->vt = &g_mem_confirmed_vt;
    store->ctx = rows;
    *out = store;
    return IDX_OK;
}

void idx_mem_confirmed_store_counts(const idx_confirmed_store *store,
                                    idx_store_counts *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (store == NULL || store->vt != &g_mem_confirmed_vt) {
        return;
    }
    mem_rows_counts((const mem_rows *)store->ctx, out);
}

/* ---------------------------------------------------- finalized reference --- */

/* The append-only tier plus the batching bookkeeping: `pending` counts rows
 * buffered since the last flush, `flushes` how many flushes have run. The rows
 * themselves are kept so a test can confirm nothing was dropped. */
typedef struct {
    mem_rows rows;
    size_t pending;
    size_t flushes;
} mem_finalized;

static const char *mem_finalized_name(void *ctx) {
    (void)ctx;
    return "memory-finalized";
}

static idx_status mem_finalized_append(void *ctx, const idx_store_write_set *set,
                                       idx_error *err) {
    mem_finalized *f = ctx;
    size_t before = idx_store_write_set_total(set);
    IDX_TRY(mem_rows_append(&f->rows, set, err));
    f->pending += before;
    return IDX_OK;
}

static idx_status mem_finalized_flush(void *ctx, idx_error *err) {
    (void)err;
    mem_finalized *f = ctx;
    f->pending = 0;
    f->flushes++;
    return IDX_OK;
}

static void mem_finalized_close(void *ctx) {
    mem_finalized *f = ctx;
    if (f != NULL) {
        mem_rows_free(&f->rows);
        free(f);
    }
}

static const idx_finalized_store_vt g_mem_finalized_vt = {
    .name = mem_finalized_name,
    .append = mem_finalized_append,
    .flush = mem_finalized_flush,
    .close = mem_finalized_close,
};

idx_status idx_mem_finalized_store_open(idx_finalized_store **out,
                                        idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out is null");
    }
    idx_finalized_store *store = calloc(1, sizeof(*store));
    mem_finalized *f = calloc(1, sizeof(*f));
    if (store == NULL || f == NULL) {
        free(store);
        free(f);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "finalized store allocation");
    }
    mem_rows_init(&f->rows);
    store->vt = &g_mem_finalized_vt;
    store->ctx = f;
    *out = store;
    return IDX_OK;
}

void idx_mem_finalized_store_counts(const idx_finalized_store *store,
                                    idx_store_counts *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (store == NULL || store->vt != &g_mem_finalized_vt) {
        return;
    }
    mem_rows_counts(&((const mem_finalized *)store->ctx)->rows, out);
}

size_t idx_mem_finalized_store_pending(const idx_finalized_store *store) {
    if (store == NULL || store->vt != &g_mem_finalized_vt) {
        return 0;
    }
    return ((const mem_finalized *)store->ctx)->pending;
}

size_t idx_mem_finalized_store_flushes(const idx_finalized_store *store) {
    if (store == NULL || store->vt != &g_mem_finalized_vt) {
        return 0;
    }
    return ((const mem_finalized *)store->ctx)->flushes;
}
