/*
 * Bar derivation. A bar is a fold of priced swaps over one pool and one time
 * bucket, so the tests build idx_bar_input rows directly (the price step is
 * tested in test_price) and check the OHLCV a bucket accumulates, the 1s/1m
 * split, out-of-order open/close, and the guards that keep an unpriceable or
 * untimed swap out.
 */
#include "bar.h"

#include <math.h>
#include <string.h>

#include "test.h"

static idx_pubkey key_fill(uint8_t fill) {
    idx_pubkey k;
    memset(k.bytes, fill, IDX_PUBKEY_LEN);
    return k;
}

static bool close_to(double a, double b) { return fabs(a - b) < 1e-9; }

/* A priced swap input: price `p`, quote SOL, base 6-dec, quote 9-dec, at
 * `time`, sequenced by (slot, tx, ix). */
static idx_bar_input input(idx_pubkey pool, double p, uint64_t base_raw,
                           uint64_t quote_raw, int64_t time, idx_slot slot,
                           uint16_t tx, uint16_t ix) {
    idx_bar_input in;
    memset(&in, 0, sizeof(in));
    in.pool = pool;
    in.price.priced = true;
    in.price.has_price = true;
    in.price.quote = IDX_QUOTE_SOL;
    in.price.price = p;
    in.price.base_amount = base_raw;
    in.price.base_decimals = 6;
    in.price.quote_amount = quote_raw;
    in.price.quote_decimals = 9;
    in.block_time = time;
    in.seq.slot = slot;
    in.seq.transaction_index = tx;
    in.seq.instruction_index = ix;
    return in;
}

static idx_status obs(idx_bar_registry *reg, const idx_bar_input *in) {
    idx_error err;
    idx_error_clear(&err);
    return idx_bar_registry_observe(reg, in, &err);
}

/* Three swaps in the same second accumulate one 1s bar: open first, high/low the
 * extremes, close last, volume summed. */
static void test_ohlcv_one_bucket(void) {
    idx_bar_registry reg;
    idx_bar_registry_init(&reg);
    idx_pubkey pool = key_fill(0x30);

    /* time 1_000_000_000 -> 1s bucket == that second; 1m bucket floors to /60. */
    idx_bar_input a = input(pool, 2.0, 1000000, 500000000, 1000000000, 10, 0, 0);
    idx_bar_input b = input(pool, 3.0, 2000000, 500000000, 1000000000, 10, 1, 0);
    idx_bar_input c = input(pool, 1.5, 1000000, 500000000, 1000000000, 10, 2, 0);
    TEST_EQ_INT(obs(&reg, &a), IDX_OK);
    TEST_EQ_INT(obs(&reg, &b), IDX_OK);
    TEST_EQ_INT(obs(&reg, &c), IDX_OK);

    const idx_bar *bar =
        idx_bar_registry_get(&reg, &pool, IDX_BAR_1S, 1000000000);
    TEST_ASSERT(bar != NULL);
    TEST_ASSERT(close_to(bar->open, 2.0));
    TEST_ASSERT(close_to(bar->high, 3.0));
    TEST_ASSERT(close_to(bar->low, 1.5));
    TEST_ASSERT(close_to(bar->close, 1.5));
    TEST_EQ_UINT(bar->swap_count, 3);
    TEST_EQ_INT(bar->quote, IDX_QUOTE_SOL);
    /* base volume: (1.0 + 2.0 + 1.0) tokens at 6 dec; quote: 1.5 SOL. */
    TEST_ASSERT(close_to(bar->base_volume, 4.0));
    TEST_ASSERT(close_to(bar->quote_volume, 1.5));

    idx_bar_registry_free(&reg);
}

/* One swap produces both a 1s and a 1m bar, bucketed independently. */
static void test_1s_and_1m(void) {
    idx_bar_registry reg;
    idx_bar_registry_init(&reg);
    idx_pubkey pool = key_fill(0x30);

    idx_bar_input a = input(pool, 2.0, 1000000, 500000000, 1000000037, 10, 0, 0);
    TEST_EQ_INT(obs(&reg, &a), IDX_OK);
    TEST_EQ_UINT(idx_bar_registry_count(&reg), 2); /* one 1s, one 1m */

    TEST_ASSERT(idx_bar_registry_get(&reg, &pool, IDX_BAR_1S, 1000000037) != NULL);
    /* 1m bucket floors 1000000037 to 1000000020 (== /60*60). */
    const idx_bar *m = idx_bar_registry_get(&reg, &pool, IDX_BAR_1M, 1000000020);
    TEST_ASSERT(m != NULL && close_to(m->open, 2.0));

    /* A second swap 20s later shares the 1m bar but gets its own 1s bar. */
    idx_bar_input b = input(pool, 4.0, 1000000, 500000000, 1000000057, 11, 0, 0);
    TEST_EQ_INT(obs(&reg, &b), IDX_OK);
    m = idx_bar_registry_get(&reg, &pool, IDX_BAR_1M, 1000000020);
    TEST_ASSERT(close_to(m->open, 2.0) && close_to(m->close, 4.0));
    TEST_EQ_UINT(m->swap_count, 2);
    TEST_EQ_UINT(idx_bar_registry_count(&reg), 3); /* 2x 1s + 1x 1m */

    idx_bar_registry_free(&reg);
}

/* Open and close follow execution order, not arrival order: a later-arriving
 * earlier-slot swap still becomes the open. */
static void test_out_of_order_open_close(void) {
    idx_bar_registry reg;
    idx_bar_registry_init(&reg);
    idx_pubkey pool = key_fill(0x30);

    /* The slot-11 swap arrives first, then the slot-10 one (a backfill). */
    idx_bar_input late = input(pool, 5.0, 1000000, 500000000, 1000000000, 11, 0, 0);
    idx_bar_input early = input(pool, 2.0, 1000000, 500000000, 1000000000, 10, 0, 0);
    TEST_EQ_INT(obs(&reg, &late), IDX_OK);
    TEST_EQ_INT(obs(&reg, &early), IDX_OK);

    const idx_bar *bar =
        idx_bar_registry_get(&reg, &pool, IDX_BAR_1S, 1000000000);
    TEST_ASSERT(close_to(bar->open, 2.0));  /* slot 10, the earlier */
    TEST_ASSERT(close_to(bar->close, 5.0)); /* slot 11, the later */
    TEST_ASSERT(close_to(bar->high, 5.0) && close_to(bar->low, 2.0));

    idx_bar_registry_free(&reg);
}

/* Different pools never share a bar, even in the same bucket (D5). */
static void test_pools_are_separate(void) {
    idx_bar_registry reg;
    idx_bar_registry_init(&reg);
    idx_pubkey p1 = key_fill(0x30);
    idx_pubkey p2 = key_fill(0x31);
    idx_bar_input a = input(p1, 2.0, 1000000, 500000000, 1000000000, 10, 0, 0);
    idx_bar_input b = input(p2, 9.0, 1000000, 500000000, 1000000000, 10, 0, 0);
    TEST_EQ_INT(obs(&reg, &a), IDX_OK);
    TEST_EQ_INT(obs(&reg, &b), IDX_OK);
    TEST_ASSERT(close_to(idx_bar_registry_get(&reg, &p1, IDX_BAR_1S, 1000000000)->open, 2.0));
    TEST_ASSERT(close_to(idx_bar_registry_get(&reg, &p2, IDX_BAR_1S, 1000000000)->open, 9.0));
    idx_bar_registry_free(&reg);
}

/* An unpriced swap or one without a block time produces no bar. */
static void test_guards(void) {
    idx_bar_registry reg;
    idx_bar_registry_init(&reg);
    idx_pubkey pool = key_fill(0x30);

    idx_bar_input unpriced = input(pool, 2.0, 1000000, 500000000, 1000000000, 10, 0, 0);
    unpriced.price.has_price = false;
    TEST_EQ_INT(obs(&reg, &unpriced), IDX_OK);

    idx_bar_input untimed = input(pool, 2.0, 1000000, 500000000, 0, 10, 0, 0);
    TEST_EQ_INT(obs(&reg, &untimed), IDX_OK);

    TEST_EQ_UINT(idx_bar_registry_count(&reg), 0);
    idx_bar_registry_free(&reg);
}

static void test_interval_names(void) {
    TEST_EQ_STR(idx_bar_interval_name(IDX_BAR_1S), "1s");
    TEST_EQ_STR(idx_bar_interval_name(IDX_BAR_1M), "1m");
    TEST_EQ_UINT(idx_bar_interval_seconds(IDX_BAR_1S), 1);
    TEST_EQ_UINT(idx_bar_interval_seconds(IDX_BAR_1M), 60);
}

TEST_MAIN({
    TEST_RUN(test_ohlcv_one_bucket);
    TEST_RUN(test_1s_and_1m);
    TEST_RUN(test_out_of_order_open_close);
    TEST_RUN(test_pools_are_separate);
    TEST_RUN(test_guards);
    TEST_RUN(test_interval_names);
})
