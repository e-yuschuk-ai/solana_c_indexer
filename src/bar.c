#include "bar.h"

#include <stdlib.h>
#include <string.h>

const char *idx_bar_interval_name(idx_bar_interval interval) {
    switch (interval) {
    case IDX_BAR_1S:
        return "1s";
    case IDX_BAR_1M:
        return "1m";
    case IDX_BAR_INTERVAL_COUNT:
        break;
    }
    return "unknown";
}

uint32_t idx_bar_interval_seconds(idx_bar_interval interval) {
    switch (interval) {
    case IDX_BAR_1S:
        return 1;
    case IDX_BAR_1M:
        return 60;
    case IDX_BAR_INTERVAL_COUNT:
        break;
    }
    return 0;
}

int idx_bar_seq_compare(const idx_bar_seq *a, const idx_bar_seq *b) {
    if (a->slot != b->slot) {
        return a->slot < b->slot ? -1 : 1;
    }
    if (a->transaction_index != b->transaction_index) {
        return a->transaction_index < b->transaction_index ? -1 : 1;
    }
    if (a->instruction_index != b->instruction_index) {
        return a->instruction_index < b->instruction_index ? -1 : 1;
    }
    /* A top-level instruction runs before the inner ones it expands into. */
    if (a->inner != b->inner) {
        return a->inner ? 1 : -1;
    }
    if (a->inner_index != b->inner_index) {
        return a->inner_index < b->inner_index ? -1 : 1;
    }
    return 0;
}

void idx_bar_registry_init(idx_bar_registry *reg) {
    if (reg != NULL) {
        idx_map_init(&reg->by_key);
    }
}

void idx_bar_registry_free(idx_bar_registry *reg) {
    if (reg == NULL) {
        return;
    }
    size_t cursor = 0;
    idx_slice key;
    void *value = NULL;
    while (idx_map_next(&reg->by_key, &cursor, &key, &value)) {
        free(value);
    }
    idx_map_free(&reg->by_key);
}

/* 10^n as a double, for a mint's decimals — a small exponent, so a repeated
 * multiply is exact within the range doubles represent and needs no libm. */
static double pow10_double(uint8_t n) {
    double result = 1.0;
    for (uint8_t i = 0; i < n; i++) {
        result *= 10.0;
    }
    return result;
}

/* The unix second the interval containing `time` starts at. */
static int64_t bucket_of(int64_t time, idx_bar_interval interval) {
    int64_t width = (int64_t)idx_bar_interval_seconds(interval);
    return (time / width) * width;
}

/*
 * The map key for a bar: the pool, the interval and the bucket, packed so no two
 * distinct bars collide. 41 bytes, past the map's inline-key size, which the map
 * handles by spilling to the heap.
 */
#define BAR_KEY_LEN (IDX_PUBKEY_LEN + 1 + 8)

static idx_slice bar_key(const idx_pubkey *pool, idx_bar_interval interval,
                        int64_t bucket, uint8_t buf[BAR_KEY_LEN]) {
    memcpy(buf, pool->bytes, IDX_PUBKEY_LEN);
    buf[IDX_PUBKEY_LEN] = (uint8_t)interval;
    uint64_t b = (uint64_t)bucket;
    for (size_t i = 0; i < 8; i++) {
        buf[IDX_PUBKEY_LEN + 1 + i] = (uint8_t)((b >> (8 * i)) & 0xff);
    }
    return idx_slice_make(buf, BAR_KEY_LEN);
}

/* Folds one swap into the bar for one interval. */
static idx_status observe_interval(idx_bar_registry *reg,
                                  const idx_bar_input *in,
                                  idx_bar_interval interval, double price,
                                  double base_vol, double quote_vol,
                                  idx_error *err) {
    int64_t bucket = bucket_of(in->block_time, interval);
    uint8_t keybuf[BAR_KEY_LEN];
    idx_slice key = bar_key(&in->pool, interval, bucket, keybuf);

    void *value = NULL;
    if (idx_map_get(&reg->by_key, key, &value)) {
        idx_bar *bar = value;
        if (price > bar->high) {
            bar->high = price;
        }
        if (price < bar->low) {
            bar->low = price;
        }
        if (idx_bar_seq_compare(&in->seq, &bar->open_seq) < 0) {
            bar->open = price;
            bar->open_seq = in->seq;
        }
        if (idx_bar_seq_compare(&in->seq, &bar->close_seq) > 0) {
            bar->close = price;
            bar->close_seq = in->seq;
        }
        bar->base_volume += base_vol;
        bar->quote_volume += quote_vol;
        bar->swap_count++;
        return IDX_OK;
    }

    idx_bar *bar = calloc(1, sizeof(*bar));
    if (bar == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "out of memory");
    }
    bar->pool = in->pool;
    bar->interval = interval;
    bar->bucket = bucket;
    bar->quote = in->price.quote;
    bar->open = bar->high = bar->low = bar->close = price;
    bar->base_volume = base_vol;
    bar->quote_volume = quote_vol;
    bar->swap_count = 1;
    bar->open_seq = in->seq;
    bar->close_seq = in->seq;
    idx_status status = idx_map_put(&reg->by_key, key, bar, err);
    if (status != IDX_OK) {
        free(bar);
        return status;
    }
    return IDX_OK;
}

/* An input's price and its two scaled volumes, the numbers a bar folds in. */
static void input_metrics(const idx_bar_input *in, double *price,
                         double *base_vol, double *quote_vol) {
    *price = in->price.price;
    *base_vol =
        (double)in->price.base_amount / pow10_double(in->price.base_decimals);
    *quote_vol =
        (double)in->price.quote_amount / pow10_double(in->price.quote_decimals);
}

/* True when an input can be folded at all: it must be priced and timed. */
static bool input_is_foldable(const idx_bar_input *in) {
    return in->price.has_price && in->block_time > 0;
}

idx_status idx_bar_registry_observe(idx_bar_registry *reg,
                                    const idx_bar_input *in, idx_error *err) {
    if (reg == NULL || in == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "reg and in must not be NULL");
    }
    /* No price means nothing to chart; no block time means nowhere to place it
     * (block time is optional on the chain). Neither is an error. */
    if (!input_is_foldable(in)) {
        return IDX_OK;
    }

    double price;
    double base_vol;
    double quote_vol;
    input_metrics(in, &price, &base_vol, &quote_vol);

    for (idx_bar_interval iv = IDX_BAR_1S; iv < IDX_BAR_INTERVAL_COUNT; iv++) {
        IDX_TRY(observe_interval(reg, in, iv, price, base_vol, quote_vol, err));
    }
    return IDX_OK;
}

const idx_bar *idx_bar_registry_get(const idx_bar_registry *reg,
                                    const idx_pubkey *pool,
                                    idx_bar_interval interval, int64_t bucket) {
    if (reg == NULL || pool == NULL) {
        return NULL;
    }
    uint8_t keybuf[BAR_KEY_LEN];
    idx_slice key = bar_key(pool, interval, bucket, keybuf);
    void *value = NULL;
    if (idx_map_get(&reg->by_key, key, &value)) {
        return (const idx_bar *)value;
    }
    return NULL;
}

size_t idx_bar_registry_count(const idx_bar_registry *reg) {
    if (reg == NULL) {
        return 0;
    }
    return idx_map_count(&reg->by_key);
}

/*
 * Folds one survivor into a single interval, but only if that interval's bucket
 * is in `affected` — so a rebuild touches exactly the buckets that were cleared
 * and never double-counts a survivor whose other bucket was left intact.
 */
static idx_status refold_if_affected(idx_bar_registry *reg,
                                    const idx_map *affected,
                                    const idx_bar_input *in,
                                    idx_bar_interval interval, double price,
                                    double base_vol, double quote_vol,
                                    idx_error *err) {
    int64_t bucket = bucket_of(in->block_time, interval);
    uint8_t keybuf[BAR_KEY_LEN];
    idx_slice key = bar_key(&in->pool, interval, bucket, keybuf);
    if (!idx_map_contains(affected, key)) {
        return IDX_OK;
    }
    return observe_interval(reg, in, interval, price, base_vol, quote_vol, err);
}

idx_status idx_bar_registry_recompute_range(idx_bar_registry *reg,
                                            idx_slot from_slot,
                                            const idx_bar_input *survivors,
                                            size_t survivor_count,
                                            size_t *dropped, idx_error *err) {
    if (reg == NULL || (survivor_count > 0 && survivors == NULL)) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "reg must not be NULL, nor survivors when counted");
    }
    if (dropped != NULL) {
        *dropped = 0;
    }

    /*
     * A bar's close is its latest swap, and a sequence orders by slot first, so
     * a bar holds a swap at or above the reorged slot exactly when its close is.
     * Those bars may now be wrong; collect their keys as the set of buckets to
     * clear and rebuild.
     */
    idx_map affected;
    idx_map_init(&affected);
    idx_status status = IDX_OK;

    size_t cursor = 0;
    idx_slice key;
    void *value = NULL;
    while (idx_map_next(&reg->by_key, &cursor, &key, &value)) {
        const idx_bar *bar = value;
        if (bar->close_seq.slot >= from_slot) {
            status = idx_map_put(&affected, key, (void *)1, err);
            if (status != IDX_OK) {
                idx_map_free(&affected);
                return status;
            }
        }
    }

    /* Clear the affected bars. Iterating the separate set leaves reg free to be
     * modified. */
    cursor = 0;
    while (idx_map_next(&affected, &cursor, &key, &value)) {
        void *bar = NULL;
        if (idx_map_get(&reg->by_key, key, &bar)) {
            free(bar);
            idx_map_remove(&reg->by_key, key);
        }
    }
    size_t cleared = idx_map_count(&affected);

    /* Rebuild only the cleared buckets, from the swaps that survive. A survivor
     * whose bucket was not affected is left alone — its bar was never cleared. */
    for (size_t i = 0; i < survivor_count && status == IDX_OK; i++) {
        const idx_bar_input *in = &survivors[i];
        if (!input_is_foldable(in)) {
            continue;
        }
        double price;
        double base_vol;
        double quote_vol;
        input_metrics(in, &price, &base_vol, &quote_vol);
        for (idx_bar_interval iv = IDX_BAR_1S; iv < IDX_BAR_INTERVAL_COUNT;
             iv++) {
            status = refold_if_affected(reg, &affected, in, iv, price, base_vol,
                                        quote_vol, err);
            if (status != IDX_OK) {
                break;
            }
        }
    }

    idx_map_free(&affected);
    if (status == IDX_OK && dropped != NULL) {
        *dropped = cleared;
    }
    return status;
}
