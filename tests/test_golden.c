/*
 * Golden-file test: a real mainnet block in, the M6 entities out.
 *
 * Every other test builds its inputs by hand, which proves the decoders against
 * the bytes the author expects. This one proves them against the bytes the chain
 * actually produced: tests/golden/golden_block.json is seven real transactions
 * lifted from mainnet slot 435146411 — a pump.fun create-and-buy, two more pump
 * curve trades, two PumpSwap trades and two Raydium AMM v4 trades — run through
 * the same path the pipeline runs, and checked against facts read off the chain
 * independently (the token's mint, name, symbol and creator, and the exact
 * amounts of its first trade).
 *
 * The fixture is loaded relative to the repository root, which is where
 * `make test` runs the binaries from.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "balance.h"
#include "bar.h"
#include "block.h"
#include "json.h"
#include "pool.h"
#include "price.h"
#include "test.h"
#include "token.h"
#include "venue.h"
#include "vote_filter.h"

#define GOLDEN_PATH "tests/golden/golden_block.json"
#define GOLDEN_SLOT 435146411ULL
#define GOLDEN_TX_COUNT 7

/*
 * The entities the seven transactions decode to, fixed by the fixture: five
 * pool swaps (the create-and-buy, two more pump curve trades, and two Raydium
 * v4 trades — the two PumpSwap transactions selected turned out to carry no
 * trade event), all five priced against SOL, over four distinct pools; eight
 * distinct token mints across the balances; and eight bars (the 1s and 1m of
 * the priced swaps, sharing buckets where they fall in the same second/minute).
 */
#define GOLDEN_EXPECTED_SWAPS 5
#define GOLDEN_EXPECTED_PRICED 5
#define GOLDEN_EXPECTED_POOLS 4
#define GOLDEN_EXPECTED_TOKENS 8
#define GOLDEN_EXPECTED_BARS 8

/* Reads the whole file into a NUL-terminated arena buffer. */
static char *read_file(idx_arena *arena, const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    void *raw = NULL;
    idx_error err;
    idx_error_clear(&err);
    if (idx_arena_alloc(arena, (size_t)size + 1, &raw, &err) != IDX_OK) {
        fclose(f);
        return NULL;
    }
    char *buf = raw;
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

/* What one run over the block accumulates. */
typedef struct {
    idx_quote_set quotes;
    idx_pool_registry pools;
    idx_token_registry tokens;
    idx_bar_registry bars;
    size_t swaps;
    size_t priced;
    size_t decode_errors;
} run_state;

/* Walks a transaction's creations and metadata into the dimensions. */
static void observe_dimensions(const idx_transaction *tx, run_state *st,
                               idx_slot slot) {
    if (!tx->has_meta || !tx->success) {
        return;
    }
    const idx_instruction *lists[1] = {tx->instructions};
    size_t counts[1] = {tx->instruction_count};
    for (size_t l = 0; l < 1; l++) {
        for (size_t i = 0; i < counts[l]; i++) {
            const idx_instruction *ix = &lists[l][i];
            idx_pool_creation c;
            if (idx_venue_creation_decode(tx, ix, &c, NULL) == IDX_OK) {
                idx_pool_registry_observe_creation(&st->pools, &c, slot, NULL);
            }
            idx_token_metadata m;
            if (idx_venue_metadata_decode(tx, ix, &m, NULL) == IDX_OK) {
                idx_token_registry_observe_metadata(&st->tokens, &m, slot, NULL);
            }
        }
    }
    for (size_t g = 0; g < tx->inner_instruction_count; g++) {
        const idx_inner_instructions *grp = &tx->inner_instructions[g];
        for (size_t j = 0; j < grp->instruction_count; j++) {
            const idx_instruction *ix = &grp->instructions[j];
            idx_pool_creation c;
            if (idx_venue_creation_decode(tx, ix, &c, NULL) == IDX_OK) {
                idx_pool_registry_observe_creation(&st->pools, &c, slot, NULL);
            }
            idx_token_metadata m;
            if (idx_venue_metadata_decode(tx, ix, &m, NULL) == IDX_OK) {
                idx_token_registry_observe_metadata(&st->tokens, &m, slot, NULL);
            }
        }
    }
}

static void process_tx(const idx_transaction *tx, run_state *st, idx_arena *arena,
                       idx_slot slot, int64_t block_time, bool has_time,
                       uint16_t tx_index) {
    if (idx_vote_filter_should_drop(tx)) {
        return;
    }

    /* Token dimension from balances. */
    const idx_token_balance_state *tb = NULL;
    size_t tb_count = 0;
    if (idx_token_balance_extract(tx, arena, &tb, &tb_count, NULL) != IDX_OK) {
        st->decode_errors++;
    }
    for (size_t i = 0; i < tb_count; i++) {
        idx_token_registry_observe_balance(&st->tokens, &tb[i].mint,
                                           tb[i].decimals, slot, NULL);
    }

    /* Swaps, prices, pool dimension and bars. */
    const idx_swap_row *rows = NULL;
    size_t count = 0;
    if (idx_swap_normalize(tx, arena, &rows, &count, NULL) != IDX_OK) {
        st->decode_errors++;
        return;
    }
    for (size_t i = 0; i < count; i++) {
        const idx_swap_row *row = &rows[i];
        st->swaps++;
        idx_price price;
        bool priced =
            idx_price_of_swap(&st->quotes, row, &price) && price.has_price;
        if (priced) {
            st->priced++;
        }
        idx_pool_registry_observe_swap(&st->pools, row, slot, NULL);
        if (priced && row->kind == IDX_SWAP_POOL && row->has_pool && has_time) {
            idx_bar_input in;
            memset(&in, 0, sizeof(in));
            in.pool = row->pool;
            in.price = price;
            in.block_time = block_time;
            in.seq.slot = slot;
            in.seq.transaction_index = tx_index;
            in.seq.instruction_index = row->instruction_index;
            in.seq.inner_index = row->inner_index;
            in.seq.inner = row->inner;
            idx_bar_registry_observe(&st->bars, &in, NULL);
        }
    }

    observe_dimensions(tx, st, slot);
}

static idx_pubkey key_from_b58(const char *text) {
    idx_pubkey k;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_pubkey_from_base58(text, strlen(text), &k, &err), IDX_OK);
    return k;
}

static void test_golden_block(void) {
    idx_arena arena;
    idx_arena_init(&arena, 0);

    size_t len = 0;
    char *json = read_file(&arena, GOLDEN_PATH, &len);
    TEST_CHECK(json != NULL, "cannot open %s (run from the repo root)",
               GOLDEN_PATH);
    if (json == NULL) {
        idx_arena_destroy(&arena);
        return;
    }

    idx_json_doc *doc = NULL;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_json_parse(idx_slice_make(json, len), &doc, &err), IDX_OK);

    idx_block block;
    TEST_EQ_INT(idx_block_decode(idx_json_root(doc), GOLDEN_SLOT, &arena, &block,
                                 &err),
                IDX_OK);
    TEST_EQ_UINT(block.transaction_count, GOLDEN_TX_COUNT);
    TEST_ASSERT(block.has_block_time);

    run_state st;
    memset(&st, 0, sizeof(st));
    idx_quote_set_defaults(&st.quotes);
    idx_pool_registry_init(&st.pools);
    idx_token_registry_init(&st.tokens);
    idx_bar_registry_init(&st.bars);

    for (size_t i = 0; i < block.transaction_count; i++) {
        process_tx(&block.transactions[i], &st, &arena, GOLDEN_SLOT,
                   block.block_time, block.has_block_time, (uint16_t)i);
    }

    /* Real data must flow through the decoders without a single failure. */
    TEST_EQ_UINT(st.decode_errors, 0);

    /* Ground truth for the created token, read off the chain independently. */
    idx_pubkey mint = key_from_b58("VpZwSVGmtgP1FH9tRTFz4iyCWzUqH8ksjVxrai6pump");
    idx_pubkey creator =
        key_from_b58("2wKNSZm8VvhZT8ZEkryAzY6dJxTxzSbq2vC6DaBmXRMY");

    const idx_token *tok = idx_token_registry_get(&st.tokens, &mint);
    TEST_ASSERT(tok != NULL);
    if (tok != NULL) {
        TEST_ASSERT(tok->has_decimals && tok->decimals == 6);
        TEST_ASSERT(tok->has_name);
        TEST_EQ_STR(tok->name, "United States Water Reserve");
        TEST_ASSERT(tok->has_symbol);
        TEST_EQ_STR(tok->symbol, "USWR");
        TEST_ASSERT(tok->has_uri); /* the URI is stored unresolved (D5) */
    }

    const idx_pool *pool = idx_pool_registry_get(&st.pools, &mint);
    TEST_ASSERT(pool != NULL);
    if (pool != NULL) {
        TEST_EQ_INT(pool->venue, IDX_VENUE_PUMP_CURVE);
        TEST_EQ_UINT(pool->first_seen_slot, GOLDEN_SLOT);
        TEST_ASSERT(pool->has_creation);
        TEST_ASSERT(pool->has_creator &&
                    idx_pubkey_equal(&pool->creator, &creator));
    }

    /*
     * The create-and-buy trade: 50000001 lamports for 1785357772752 base units,
     * a pump token at 6 decimals against SOL at 9. The price a bar records is
     * quote-per-base = (50000001/1e9) / (1785357772752/1e6).
     */
    double expected_price = (50000001.0 / 1e9) / (1785357772752.0 / 1e6);
    int64_t bucket = (block.block_time / 1) * 1;
    const idx_bar *bar =
        idx_bar_registry_get(&st.bars, &mint, IDX_BAR_1S, bucket);
    TEST_ASSERT(bar != NULL);
    if (bar != NULL) {
        TEST_EQ_INT(bar->quote, IDX_QUOTE_SOL);
        double rel = (bar->open - expected_price) / expected_price;
        TEST_CHECK(rel > -1e-6 && rel < 1e-6,
                   "bar open %.12g != expected %.12g", bar->open,
                   expected_price);
    }

    /* Aggregate shape of the seven transactions, from a trusted first run. */
    TEST_EQ_UINT(st.swaps, GOLDEN_EXPECTED_SWAPS);
    TEST_EQ_UINT(st.priced, GOLDEN_EXPECTED_PRICED);
    TEST_EQ_UINT(idx_pool_registry_count(&st.pools), GOLDEN_EXPECTED_POOLS);
    TEST_EQ_UINT(idx_token_registry_count(&st.tokens), GOLDEN_EXPECTED_TOKENS);
    TEST_EQ_UINT(idx_bar_registry_count(&st.bars), GOLDEN_EXPECTED_BARS);

    idx_pool_registry_free(&st.pools);
    idx_token_registry_free(&st.tokens);
    idx_bar_registry_free(&st.bars);
    idx_json_free(doc);
    idx_arena_destroy(&arena);
}

TEST_MAIN({ TEST_RUN(test_golden_block); })
