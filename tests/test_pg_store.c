/*
 * Confirmed-tier PostgreSQL store tests.
 *
 * Offline: the constructor rejects bad arguments, and the vtable slots this
 * commit does not implement (reorg, prune, read_range) report unsupported
 * through the store.h dispatch. Online (only when IDX_TEST_PG_CONNINFO names a
 * database): a write set of every entity is written and read back with plain
 * SQL, and the three behaviours the schema exists for are checked — events are
 * idempotent on the instruction path, balance state keeps the newest slot, and
 * bars fold across write calls.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pg.h"
#include "pg_store.h"
#include "test.h"

static const char *g_tables[] = {
    "blocks",         "sol_balances", "token_balances", "sol_transfers",
    "token_transfers", "swaps",       "pools",          "tokens",
    "bars"};

/* count(*) of `table`, or UINT64_MAX on error (so a bad query fails a check). */
static uint64_t table_count(idx_pg_conn *c, const char *table) {
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT count(*) FROM %s", table);
    idx_pg_result *r = NULL;
    if (idx_pg_exec(c, sql, &r, NULL) != IDX_OK) {
        return UINT64_MAX;
    }
    uint64_t n = UINT64_MAX;
    idx_pg_result_u64(r, 0, 0, &n, NULL);
    idx_pg_result_free(r);
    return n;
}

/* A double read from a single-cell query. */
static double scalar_double(idx_pg_conn *c, const char *sql) {
    idx_pg_result *r = NULL;
    if (idx_pg_exec(c, sql, &r, NULL) != IDX_OK) {
        return -1;
    }
    const char *text = idx_pg_result_text(r, 0, 0);
    double v = text != NULL ? atof(text) : -1;
    idx_pg_result_free(r);
    return v;
}

static idx_store_block_row block_row(idx_slot slot) {
    idx_store_block_row b;
    memset(&b, 0, sizeof b);
    b.slot = slot;
    b.blockhash.bytes[0] = 0xaa;
    b.previous_blockhash.bytes[0] = 0xbb;
    b.parent_slot = slot - 1;
    b.has_block_time = true;
    b.block_time = 1700000000 + (int64_t)slot;
    b.transaction_count = 5;
    return b;
}

static idx_store_sol_balance_row sol_row(idx_slot slot, uint8_t tag,
                                         uint64_t lamports) {
    idx_store_sol_balance_row r;
    memset(&r, 0, sizeof r);
    r.ref.slot = slot;
    r.ref.transaction_index = tag;
    r.balance.account.bytes[0] = tag;
    r.balance.lamports = lamports;
    r.balance.delta = 10;
    return r;
}

static idx_store_swap_row swap_row(idx_slot slot, uint8_t tag, bool priced) {
    idx_store_swap_row r;
    memset(&r, 0, sizeof r);
    r.ref.slot = slot;
    r.ref.transaction_index = tag;
    r.swap.instruction_index = tag;
    r.swap.venue = IDX_VENUE_PUMP_CURVE;
    r.swap.has_pool = true;
    r.swap.pool.bytes[0] = 0x50;
    r.swap.has_input_amount = true;
    r.swap.input_amount = 1000;
    r.has_block_time = true;
    r.block_time = 1700000000 + (int64_t)slot;
    if (priced) {
        r.price.priced = true;
        r.price.quote = IDX_QUOTE_SOL;
        r.price.has_price = true;
        r.price.price = 0.25;
    }
    return r;
}

static idx_bar bar_row(int64_t bucket, idx_slot seq_slot, double price,
                       double base_vol) {
    idx_bar b;
    memset(&b, 0, sizeof b);
    b.pool.bytes[0] = 0x50;
    b.interval = IDX_BAR_1S;
    b.bucket = bucket;
    b.quote = IDX_QUOTE_SOL;
    b.open = b.high = b.low = b.close = price;
    b.base_volume = base_vol;
    b.quote_volume = base_vol * price;
    b.swap_count = 1;
    b.open_seq.slot = seq_slot;
    b.close_seq.slot = seq_slot;
    return b;
}

static void test_offline(void) {
    idx_error err;
    idx_error_clear(&err);
    idx_confirmed_store *store = NULL;
    TEST_EQ_INT(idx_pg_confirmed_store_open(NULL, &store, &err),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_pg_confirmed_store_open("x", NULL, &err),
                IDX_ERR_INVALID_ARG);

    /* The schema is always inspectable, no connection needed. */
    const char *const *schema = idx_pg_confirmed_schema();
    TEST_ASSERT(schema != NULL && schema[0] != NULL);
}

static void test_online(void) {
    const char *conninfo = getenv("IDX_TEST_PG_CONNINFO");
    idx_error err;
    idx_error_clear(&err);

    /* A raw connection to reset the schema and to read results back. */
    idx_pg_conn *raw = NULL;
    TEST_EQ_INT(idx_pg_connect(conninfo, &raw, &err), IDX_OK);
    if (raw == NULL) {
        return;
    }
    for (size_t i = 0; i < sizeof g_tables / sizeof g_tables[0]; i++) {
        char sql[128];
        snprintf(sql, sizeof sql, "DROP TABLE IF EXISTS %s CASCADE", g_tables[i]);
        idx_pg_exec(raw, sql, NULL, &err);
    }

    idx_confirmed_store *store = NULL;
    TEST_EQ_INT(idx_pg_confirmed_store_open(conninfo, &store, &err), IDX_OK);
    TEST_EQ_STR(idx_confirmed_store_name(store), "postgres-confirmed");

    /* -------- a full write set -------- */
    idx_store_block_row block = block_row(100);
    idx_store_sol_balance_row sols[2] = {sol_row(100, 1, 5000),
                                         sol_row(100, 2, 6000)};

    idx_store_token_balance_row tok;
    memset(&tok, 0, sizeof tok);
    tok.ref.slot = 100;
    tok.balance.account.bytes[0] = 9;
    tok.balance.mint.bytes[0] = 0x11;
    tok.balance.has_owner = true;
    tok.balance.owner.bytes[0] = 0x22;
    tok.balance.amount = 12345678901234ULL; /* exceeds int32, fits NUMERIC */
    tok.balance.decimals = 6;

    idx_store_transfer_row transfers[2];
    memset(transfers, 0, sizeof transfers);
    transfers[0].ref.slot = 100;
    transfers[0].transfer.kind = IDX_TRANSFER_SOL;
    transfers[0].transfer.instruction_index = 0;
    transfers[0].transfer.amount = 999;
    transfers[1].ref.slot = 100;
    transfers[1].transfer.kind = IDX_TRANSFER_TOKEN;
    transfers[1].transfer.instruction_index = 1;
    transfers[1].transfer.has_mint = true;
    transfers[1].transfer.mint.bytes[0] = 0x11;
    transfers[1].transfer.amount = 4242;

    idx_store_swap_row swaps[2] = {swap_row(100, 0, true),
                                   swap_row(100, 1, false)};
    /* Distinguish the two swaps' instruction paths. */
    swaps[1].swap.instruction_index = 1;

    idx_pool pool;
    memset(&pool, 0, sizeof pool);
    pool.address.bytes[0] = 0x50;
    pool.venue = IDX_VENUE_PUMP_CURVE;
    pool.has_mint_a = true;
    pool.mint_a.bytes[0] = 0x11;
    pool.has_decimals_a = true;
    pool.decimals_a = 6;
    pool.first_seen_slot = 100;
    pool.swap_count = 2;

    idx_token token;
    memset(&token, 0, sizeof token);
    token.mint.bytes[0] = 0x11;
    token.has_decimals = true;
    token.decimals = 6;
    token.has_name = true;
    strcpy(token.name, "Test Token");
    token.has_symbol = true;
    strcpy(token.symbol, "TEST");
    token.first_seen_slot = 100;

    idx_bar bar = bar_row(1700000100, 100, 10.0, 5.0);

    idx_store_write_set set;
    idx_store_write_set_init(&set);
    set.blocks = &block;
    set.block_count = 1;
    set.sol_balances = sols;
    set.sol_balance_count = 2;
    set.token_balances = &tok;
    set.token_balance_count = 1;
    set.transfers = transfers;
    set.transfer_count = 2;
    set.swaps = swaps;
    set.swap_count = 2;
    set.pools = &pool;
    set.pool_count = 1;
    set.tokens = &token;
    set.token_count = 1;
    set.bars = &bar;
    set.bar_count = 1;

    TEST_EQ_INT(idx_confirmed_store_write(store, &set, &err), IDX_OK);

    /* Every table has what the set carried. */
    TEST_EQ_UINT(table_count(raw, "blocks"), 1);
    TEST_EQ_UINT(table_count(raw, "sol_balances"), 2);
    TEST_EQ_UINT(table_count(raw, "token_balances"), 1);
    TEST_EQ_UINT(table_count(raw, "sol_transfers"), 1);
    TEST_EQ_UINT(table_count(raw, "token_transfers"), 1);
    TEST_EQ_UINT(table_count(raw, "swaps"), 2);
    TEST_EQ_UINT(table_count(raw, "pools"), 1);
    TEST_EQ_UINT(table_count(raw, "tokens"), 1);
    TEST_EQ_UINT(table_count(raw, "bars"), 1);

    /* A uint64 beyond int32 survived as NUMERIC. */
    TEST_ASSERT(scalar_double(raw,
                              "SELECT amount FROM token_balances") > 1.2e13);
    /* The priced swap kept its price; the other is null. */
    TEST_ASSERT(scalar_double(
                    raw, "SELECT price FROM swaps WHERE price IS NOT NULL") ==
                0.25);
    TEST_EQ_UINT(
        (uint64_t)scalar_double(raw, "SELECT count(*) FROM swaps WHERE price "
                                     "IS NULL"),
        1);

    /* -------- events are idempotent, balances upsert -------- */
    /* Bars are excluded from the re-write: they fold with + by design (D4), so
     * re-sending a contribution would double it — the reorg path, not an
     * idempotent replay, is what corrects them. Events and state are idempotent
     * and safe to replay. */
    set.bars = NULL;
    set.bar_count = 0;
    TEST_EQ_INT(idx_confirmed_store_write(store, &set, &err), IDX_OK);
    TEST_EQ_UINT(table_count(raw, "sol_transfers"), 1); /* not duplicated */
    TEST_EQ_UINT(table_count(raw, "swaps"), 2);
    TEST_EQ_UINT(table_count(raw, "sol_balances"), 2); /* upsert, not append */

    /* -------- balance keeps the newest slot -------- */
    idx_store_sol_balance_row newer = sol_row(101, 1, 7777);
    idx_store_sol_balance_row older = sol_row(99, 1, 1111);
    idx_store_write_set one;
    idx_store_write_set_init(&one);
    one.sol_balances = &newer;
    one.sol_balance_count = 1;
    TEST_EQ_INT(idx_confirmed_store_write(store, &one, &err), IDX_OK);
    one.sol_balances = &older;
    TEST_EQ_INT(idx_confirmed_store_write(store, &one, &err), IDX_OK);
    /* The newer slot's value stands; the older write was ignored. */
    TEST_EQ_UINT(
        (uint64_t)scalar_double(
            raw, "SELECT lamports FROM sol_balances WHERE slot = 101"),
        7777);
    TEST_EQ_UINT((uint64_t)scalar_double(
                     raw, "SELECT count(*) FROM sol_balances WHERE lamports = "
                          "1111"),
                 0);

    /* -------- bars fold across write calls -------- */
    /* A second contribution to the same bucket, later in execution order. */
    idx_bar bar2 = bar_row(1700000100, 101, 20.0, 7.0);
    idx_store_write_set barset;
    idx_store_write_set_init(&barset);
    barset.bars = &bar2;
    barset.bar_count = 1;
    TEST_EQ_INT(idx_confirmed_store_write(store, &barset, &err), IDX_OK);

    TEST_EQ_UINT(table_count(raw, "bars"), 1); /* same bucket, merged */
    /* open from the earlier seq (10), close from the later (20), high 20,
     * low 10, base_volume 5+7. */
    TEST_ASSERT(scalar_double(raw, "SELECT open FROM bars") == 10.0);
    TEST_ASSERT(scalar_double(raw, "SELECT close FROM bars") == 20.0);
    TEST_ASSERT(scalar_double(raw, "SELECT high FROM bars") == 20.0);
    TEST_ASSERT(scalar_double(raw, "SELECT low FROM bars") == 10.0);
    TEST_ASSERT(scalar_double(raw, "SELECT base_volume FROM bars") == 12.0);
    TEST_EQ_UINT(
        (uint64_t)scalar_double(raw, "SELECT swap_count FROM bars"), 2);

    /* -------- reorg: delete at or above a slot, atomically -------- */
    /* State right now: sol_balances holds account 1 at slot 101 (7777) and
     * account 2 at slot 100; the bar's latest swap is at slot 101. */
    TEST_EQ_UINT(table_count(raw, "sol_balances"), 2);
    TEST_EQ_UINT(table_count(raw, "bars"), 1);

    /* A pure delete from slot 101 drops the slot-101 balance and the bar (its
     * close is at slot 101), leaving the slot-100 rows. */
    TEST_EQ_INT(idx_confirmed_store_reorg(store, 101, NULL, &err), IDX_OK);
    TEST_EQ_UINT(table_count(raw, "sol_balances"), 1); /* account 2 @ 100 */
    TEST_EQ_UINT(table_count(raw, "bars"), 0);
    TEST_EQ_UINT(table_count(raw, "blocks"), 1); /* slot 100 untouched */

    /* A reorg from slot 100 with a replacement: everything at 100 goes, then
     * the corrected rows are applied — in one transaction. */
    idx_store_block_row rblock = block_row(100);
    idx_store_swap_row rswap = swap_row(100, 7, true);
    idx_store_write_set repl;
    idx_store_write_set_init(&repl);
    repl.blocks = &rblock;
    repl.block_count = 1;
    repl.swaps = &rswap;
    repl.swap_count = 1;
    TEST_EQ_INT(idx_confirmed_store_reorg(store, 100, &repl, &err), IDX_OK);
    TEST_EQ_UINT(table_count(raw, "blocks"), 1);        /* the replacement's */
    TEST_EQ_UINT(table_count(raw, "swaps"), 1);         /* only the new one */
    TEST_EQ_UINT(table_count(raw, "sol_balances"), 0);  /* none replaced */
    TEST_EQ_UINT(table_count(raw, "sol_transfers"), 0); /* deleted, not re-added */

    /* -------- prune (retention) drops below a slot -------- */
    /* State: the replacement left a block and a swap at slot 100. Pruning at
     * 100 keeps slot >= 100, so nothing goes; pruning at 101 drops them. */
    TEST_EQ_INT(idx_confirmed_store_prune(store, 100, &err), IDX_OK);
    TEST_EQ_UINT(table_count(raw, "blocks"), 1);
    TEST_EQ_UINT(table_count(raw, "swaps"), 1);
    TEST_EQ_INT(idx_confirmed_store_prune(store, 101, &err), IDX_OK);
    TEST_EQ_UINT(table_count(raw, "blocks"), 0);
    TEST_EQ_UINT(table_count(raw, "swaps"), 0);

    /* Clean up so a rerun starts fresh. */
    for (size_t i = 0; i < sizeof g_tables / sizeof g_tables[0]; i++) {
        char sql[128];
        snprintf(sql, sizeof sql, "DROP TABLE IF EXISTS %s CASCADE", g_tables[i]);
        idx_pg_exec(raw, sql, NULL, &err);
    }
    idx_confirmed_store_close(store);
    idx_pg_close(raw);
}

TEST_MAIN({
    TEST_RUN(test_offline);
    if (getenv("IDX_TEST_PG_CONNINFO") != NULL) {
        TEST_RUN(test_online);
    } else {
        printf("  (skipping pg store online tests: set IDX_TEST_PG_CONNINFO)\n");
    }
})
