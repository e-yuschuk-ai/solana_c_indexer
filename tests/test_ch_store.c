/*
 * Finalized-tier ClickHouse store tests.
 *
 * Offline: the constructor's argument checks, the schema is inspectable with no
 * connection, and the pending counters ignore a handle this module did not
 * return.
 *
 * Online (only when IDX_TEST_CH_URL points at a server): the same write set is
 * appended through two stores, one per format, into two databases, and every
 * table is compared with EXCEPT in both directions. Then the three behaviours
 * the schema exists for are checked against the server rather than asserted
 * about the DDL text — balance state keeps the newest slot without a delete,
 * re-appending a set converges instead of duplicating, and a bar lands in the
 * table for its resolution.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "ch_store.h"
#include "test.h"

/* Every table the schema creates, for the cross-format comparison. */
static const char *const g_tables[] = {
    "blocks",   "sol_balances", "token_balances", "sol_transfers",
    "token_transfers", "swaps", "pools",          "tokens",
    "bars_1s",  "bars_1m",      "bars_1d"};
#define TABLE_COUNT (sizeof g_tables / sizeof g_tables[0])

static const char *const g_db_bin = "idx_test_ch_store_bin";
static const char *const g_db_json = "idx_test_ch_store_json";

static long long scalar(idx_ch_conn *c, const char *sql) {
    idx_ch_result *r = NULL;
    idx_error err;
    idx_error_clear(&err);
    if (idx_ch_query(c, sql, &r, &err) != IDX_OK) {
        TEST_CHECK(false, "query failed: %s (%s)", sql, err.message);
        return -1;
    }
    const char *body = idx_ch_result_body(r);
    long long v = body != NULL ? atoll(body) : -1;
    idx_ch_result_free(r);
    return v;
}

static double scalar_double(idx_ch_conn *c, const char *sql) {
    idx_ch_result *r = NULL;
    if (idx_ch_query(c, sql, &r, NULL) != IDX_OK) {
        return -1;
    }
    const char *body = idx_ch_result_body(r);
    double v = body != NULL ? atof(body) : -1;
    idx_ch_result_free(r);
    return v;
}

/* ------------------------------------------------------------- fixtures -- */

static idx_store_block_row block_row(idx_slot slot) {
    idx_store_block_row b;
    memset(&b, 0, sizeof b);
    b.slot = slot;
    b.blockhash.bytes[0] = 0xaa;
    /* A high byte in a key is what the JSONEachRow escaping rule exists for. */
    b.blockhash.bytes[31] = 0xff;
    b.previous_blockhash.bytes[0] = 0xbb;
    b.parent_slot = slot - 1;
    b.has_block_time = true;
    b.block_time = 1700000000 + (int64_t)slot;
    b.has_block_height = true;
    b.block_height = 900000 + slot;
    b.transaction_count = 5;
    return b;
}

static idx_store_sol_balance_row sol_row(idx_slot slot, uint8_t account,
                                         uint64_t lamports) {
    idx_store_sol_balance_row r;
    memset(&r, 0, sizeof r);
    r.ref.slot = slot;
    r.ref.transaction_index = 3;
    r.ref.signature.bytes[0] = 0x11;
    r.balance.account.bytes[0] = account;
    r.balance.lamports = lamports;
    r.balance.delta = -7;
    return r;
}

static idx_store_token_balance_row token_balance_row(idx_slot slot) {
    idx_store_token_balance_row r;
    memset(&r, 0, sizeof r);
    r.ref.slot = slot;
    r.ref.transaction_index = 4;
    r.ref.signature.bytes[0] = 0x22;
    r.balance.account.bytes[0] = 0x71;
    r.balance.mint.bytes[0] = 0x72;
    r.balance.has_owner = true;
    r.balance.owner.bytes[0] = 0x73;
    /* Past int64: a raw token amount is bounded only by uint64 (balance.h). */
    r.balance.amount = 18446744073709551615ull;
    r.balance.previous = 1;
    r.balance.decimals = 6;
    r.balance.closed = false;
    return r;
}

static idx_store_transfer_row transfer_row(idx_slot slot,
                                           idx_transfer_kind kind) {
    idx_store_transfer_row r;
    memset(&r, 0, sizeof r);
    r.ref.slot = slot;
    r.ref.transaction_index = 5;
    r.ref.signature.bytes[0] = 0x33;
    r.transfer.kind = kind;
    r.transfer.instruction_index = 1;
    r.transfer.source.bytes[0] = 0x81;
    r.transfer.destination.bytes[0] = 0x82;
    r.transfer.amount = 4200;
    if (kind != IDX_TRANSFER_SOL) {
        r.transfer.has_mint = true;
        r.transfer.mint.bytes[0] = 0x83;
        r.transfer.has_decimals = true;
        r.transfer.decimals = 9;
        r.transfer.fee = 7;
    }
    return r;
}

static idx_store_swap_row swap_row(idx_slot slot, uint16_t tag, bool priced) {
    idx_store_swap_row r;
    memset(&r, 0, sizeof r);
    r.ref.slot = slot;
    r.ref.transaction_index = tag;
    r.ref.signature.bytes[0] = 0x44;
    r.swap.instruction_index = tag;
    r.swap.venue = IDX_VENUE_PUMP_CURVE;
    r.swap.source = IDX_AMOUNT_EVENT;
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

static idx_pool pool_row(idx_slot slot) {
    idx_pool p;
    memset(&p, 0, sizeof p);
    p.address.bytes[0] = 0x50;
    p.venue = IDX_VENUE_PUMP_CURVE;
    p.has_mint_a = true;
    p.mint_a.bytes[0] = 0x51;
    p.has_decimals_a = true;
    p.decimals_a = 6;
    p.first_seen_slot = slot;
    p.swap_count = 2;
    return p;
}

static idx_token token_row(idx_slot slot) {
    idx_token t;
    memset(&t, 0, sizeof t);
    t.mint.bytes[0] = 0x51;
    t.has_decimals = true;
    t.decimals = 6;
    t.has_name = true;
    snprintf(t.name, sizeof t.name, "United States Water Reserve");
    t.has_symbol = true;
    snprintf(t.symbol, sizeof t.symbol, "USWR");
    t.first_seen_slot = slot;
    t.has_metadata = true;
    return t;
}

static idx_bar bar_row(idx_bar_interval interval, int64_t bucket,
                       idx_slot seq_slot, double price) {
    idx_bar b;
    memset(&b, 0, sizeof b);
    b.pool.bytes[0] = 0x50;
    b.interval = interval;
    b.bucket = bucket;
    b.quote = IDX_QUOTE_SOL;
    b.open = b.high = b.low = b.close = price;
    b.base_volume = 10.0;
    b.quote_volume = 10.0 * price;
    b.swap_count = 1;
    b.open_seq.slot = seq_slot;
    b.close_seq.slot = seq_slot;
    b.close_seq.transaction_index = 2;
    return b;
}

/*
 * One of every entity. The two SOL balances name the same account at different
 * slots, which is what the state table's version column has to resolve.
 */
typedef struct {
    idx_store_block_row blocks[1];
    idx_store_sol_balance_row sol[2];
    idx_store_token_balance_row token_balances[1];
    idx_store_transfer_row transfers[2];
    idx_store_swap_row swaps[2];
    idx_pool pools[1];
    idx_token tokens[1];
    idx_bar bars[2];
} fixture;

static void fixture_init(fixture *f, idx_store_write_set *set, idx_slot slot) {
    memset(f, 0, sizeof *f);
    f->blocks[0] = block_row(slot);
    f->sol[0] = sol_row(slot, 0x60, 100);
    f->sol[1] = sol_row(slot + 1, 0x60, 250); /* same account, newer slot */
    f->token_balances[0] = token_balance_row(slot);
    f->transfers[0] = transfer_row(slot, IDX_TRANSFER_SOL);
    f->transfers[1] = transfer_row(slot, IDX_TRANSFER_TOKEN);
    f->swaps[0] = swap_row(slot, 1, true);
    f->swaps[1] = swap_row(slot, 2, false);
    f->pools[0] = pool_row(slot);
    f->tokens[0] = token_row(slot);
    f->bars[0] = bar_row(IDX_BAR_1S, 1700000000, slot, 0.25);
    f->bars[1] = bar_row(IDX_BAR_1M, 1699999980, slot, 0.25);

    idx_store_write_set_init(set);
    set->blocks = f->blocks;
    set->block_count = 1;
    set->sol_balances = f->sol;
    set->sol_balance_count = 2;
    set->token_balances = f->token_balances;
    set->token_balance_count = 1;
    set->transfers = f->transfers;
    set->transfer_count = 2;
    set->swaps = f->swaps;
    set->swap_count = 2;
    set->pools = f->pools;
    set->pool_count = 1;
    set->tokens = f->tokens;
    set->token_count = 1;
    set->bars = f->bars;
    set->bar_count = 2;
}

/* --------------------------------------------------------------- offline -- */

static void test_offline(void) {
    idx_error err;
    idx_error_clear(&err);

    idx_ch_options opt;
    idx_ch_options_init(&opt);
    opt.url = "http://127.0.0.1:8123";

    TEST_EQ_INT(idx_ch_finalized_store_open(&opt, IDX_CH_FORMAT_ROW_BINARY,
                                            NULL, &err),
                IDX_ERR_INVALID_ARG);
    idx_finalized_store *store = NULL;
    idx_ch_options bad;
    idx_ch_options_init(&bad);
    TEST_EQ_INT(idx_ch_finalized_store_open(&bad, IDX_CH_FORMAT_ROW_BINARY,
                                            &store, &err),
                IDX_ERR_INVALID_ARG);

    /* The schema is inspectable with no connection, and covers every table. */
    const char *const *schema = idx_ch_finalized_schema();
    TEST_ASSERT(schema != NULL && schema[0] != NULL);
    size_t n = 0;
    while (schema[n] != NULL) {
        n++;
    }
    TEST_EQ_UINT(n, TABLE_COUNT);

    /* The counters ignore a handle from another backend, and NULL. */
    idx_finalized_store *mem = NULL;
    TEST_EQ_INT(idx_mem_finalized_store_open(&mem, &err), IDX_OK);
    TEST_EQ_UINT(idx_ch_finalized_store_pending(mem), 0);
    TEST_EQ_UINT(idx_ch_finalized_store_pending_bytes(mem), 0);
    idx_finalized_store_close(mem);
    TEST_EQ_UINT(idx_ch_finalized_store_pending(NULL), 0);
    TEST_EQ_UINT(idx_ch_finalized_store_pending_bytes(NULL), 0);
}

/* ---------------------------------------------------------------- online -- */

/* Opens a store on `database`, having created it fresh. */
static idx_finalized_store *open_store(idx_ch_conn *admin, const char *database,
                                       idx_ch_row_format format) {
    char sql[128];
    idx_error err;
    idx_error_clear(&err);
    snprintf(sql, sizeof sql, "DROP DATABASE IF EXISTS %s", database);
    TEST_EQ_INT(idx_ch_query(admin, sql, NULL, &err), IDX_OK);
    snprintf(sql, sizeof sql, "CREATE DATABASE %s", database);
    TEST_EQ_INT(idx_ch_query(admin, sql, NULL, &err), IDX_OK);

    idx_ch_options opt;
    idx_ch_options_init(&opt);
    opt.url = getenv("IDX_TEST_CH_URL");
    opt.database = database;
    idx_finalized_store *store = NULL;
    idx_status st = idx_ch_finalized_store_open(&opt, format, &store, &err);
    TEST_CHECK(st == IDX_OK, "open %s: %s", database, err.message);
    return store;
}

static void test_online(void) {
    idx_ch_options opt;
    idx_ch_options_init(&opt);
    opt.url = getenv("IDX_TEST_CH_URL");

    idx_error err;
    idx_error_clear(&err);
    idx_ch_conn *conn = NULL;
    TEST_EQ_INT(idx_ch_open(&opt, &conn, &err), IDX_OK);
    if (conn == NULL) {
        return;
    }

    idx_finalized_store *bin =
        open_store(conn, g_db_bin, IDX_CH_FORMAT_ROW_BINARY);
    idx_finalized_store *json =
        open_store(conn, g_db_json, IDX_CH_FORMAT_JSON_EACH_ROW);
    if (bin == NULL || json == NULL) {
        idx_ch_close(conn);
        return;
    }

    const idx_slot slot = 435146411;
    fixture f;
    idx_store_write_set set;
    fixture_init(&f, &set, slot);

    /* Append buffers; nothing is durable until flush (D3). */
    TEST_EQ_INT(idx_finalized_store_append(bin, &set, &err), IDX_OK);
    TEST_EQ_UINT(idx_ch_finalized_store_pending(bin), 12);
    TEST_ASSERT(idx_ch_finalized_store_pending_bytes(bin) > 0);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_test_ch_store_bin.swaps"),
                0);

    TEST_EQ_INT(idx_finalized_store_flush(bin, &err), IDX_OK);
    TEST_EQ_UINT(idx_ch_finalized_store_pending(bin), 0);
    TEST_EQ_UINT(idx_ch_finalized_store_pending_bytes(bin), 0);

    TEST_EQ_INT(idx_finalized_store_append(json, &set, &err), IDX_OK);
    TEST_EQ_INT(idx_finalized_store_flush(json, &err), IDX_OK);

    /*
     * Row counts per table, and that transfers split by kind (D5). Two SOL
     * balance rows were appended for one account and one row landed:
     * ReplacingMergeTree collapses rows sharing a sort key within a single
     * insert block, at insert time rather than at merge time, keeping the
     * highest version. Only duplicates spread across separate inserts wait for
     * a merge — which is what makes the FINAL check further down necessary.
     */
    char sql[256];
    const struct {
        const char *table;
        long long rows;
    } expected[] = {
        {"blocks", 1},          {"sol_balances", 1},   {"token_balances", 1},
        {"sol_transfers", 1},   {"token_transfers", 1}, {"swaps", 2},
        {"pools", 1},           {"tokens", 1},          {"bars_1s", 1},
        {"bars_1m", 1},         {"bars_1d", 0},
    };
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        snprintf(sql, sizeof sql, "SELECT count() FROM %s.%s", g_db_bin,
                 expected[i].table);
        TEST_CHECK(scalar(conn, sql) == expected[i].rows,
                   "%s: expected %lld rows", expected[i].table,
                   expected[i].rows);
    }

    /*
     * The two formats must produce the same rows. EXCEPT in both directions
     * over every table is the assertion: a difference in any value, null or key
     * byte leaves a row on one side.
     */
    for (size_t i = 0; i < TABLE_COUNT; i++) {
        snprintf(sql, sizeof sql,
                 "SELECT count() FROM (SELECT * FROM %s.%s EXCEPT SELECT * "
                 "FROM %s.%s)",
                 g_db_bin, g_tables[i], g_db_json, g_tables[i]);
        TEST_CHECK(scalar(conn, sql) == 0, "%s: binary has rows json does not",
                   g_tables[i]);
        snprintf(sql, sizeof sql,
                 "SELECT count() FROM (SELECT * FROM %s.%s EXCEPT SELECT * "
                 "FROM %s.%s)",
                 g_db_json, g_tables[i], g_db_bin, g_tables[i]);
        TEST_CHECK(scalar(conn, sql) == 0, "%s: json has rows binary does not",
                   g_tables[i]);
    }

    /* Values, not just agreement: the price, the name, and a raw amount past
     * int64 that only an unsigned column holds. */
    TEST_ASSERT(scalar_double(conn,
                              "SELECT price FROM idx_test_ch_store_bin.swaps "
                              "WHERE transaction_index = 1") == 0.25);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_test_ch_store_bin.swaps "
                             "WHERE transaction_index = 2 AND price IS NULL "
                             "AND quote IS NULL"),
                1);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_test_ch_store_bin.tokens "
                             "WHERE name = 'United States Water Reserve' AND "
                             "symbol = 'USWR'"),
                1);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM "
                             "idx_test_ch_store_bin.token_balances WHERE "
                             "amount = 18446744073709551615"),
                1);
    /* The blockhash survived both encodings, high byte included. */
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_test_ch_store_json.blocks "
                             "WHERE substring(blockhash, 32, 1) = "
                             "unhex('FF')"),
                1);

    /*
     * Balance state: two observations of one account, and the version column
     * resolves them without a delete. FINAL is how a reader sees the merged
     * view — §5.5's constraint on the query layer, made concrete.
     */
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM "
                             "idx_test_ch_store_bin.sol_balances FINAL"),
                1);
    TEST_EQ_INT(scalar(conn, "SELECT lamports FROM "
                             "idx_test_ch_store_bin.sol_balances FINAL"),
                250);

    /*
     * Re-indexing a slot is safe: appending the same set again and flushing
     * leaves the merged view unchanged, because every table is a
     * ReplacingMergeTree keyed on its sort key.
     */
    TEST_EQ_INT(idx_finalized_store_append(bin, &set, &err), IDX_OK);
    TEST_EQ_INT(idx_finalized_store_flush(bin, &err), IDX_OK);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_test_ch_store_bin.swaps "
                             "FINAL"),
                2);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM "
                             "idx_test_ch_store_bin.sol_balances FINAL"),
                1);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_test_ch_store_bin.bars_1s "
                             "FINAL"),
                1);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_test_ch_store_bin.pools "
                             "FINAL"),
                1);

    /*
     * The design facts the DDL is supposed to establish, read back from the
     * server's own metadata rather than asserted about the SQL text. A codec or
     * a PARTITION BY that stopped being applied would otherwise cost storage
     * and query time silently.
     */
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM system.tables WHERE database "
                             "= 'idx_test_ch_store_bin' AND name = 'swaps' AND "
                             "sorting_key = 'slot, transaction_index, "
                             "instruction_index, inner_index'"),
                1);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM system.tables WHERE database "
                             "= 'idx_test_ch_store_bin' AND name = 'swaps' AND "
                             "partition_key = 'intDiv(slot, 10000000)'"),
                1);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM system.tables WHERE database "
                             "= 'idx_test_ch_store_bin' AND name = "
                             "'sol_balances' AND sorting_key = 'account' AND "
                             "partition_key = ''"),
                1);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM system.tables WHERE database "
                             "= 'idx_test_ch_store_bin' AND name = 'bars_1s' "
                             "AND sorting_key = 'pool, bucket'"),
                1);
    /* Delta before ZSTD on the monotonic columns, ZSTD on the wide keys. The
     * server reports Delta with the width it resolved from the column type. */
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM system.columns WHERE database "
                             "= 'idx_test_ch_store_bin' AND table = 'swaps' AND "
                             "name = 'slot' AND compression_codec = "
                             "'CODEC(Delta(8), ZSTD(1))'"),
                1);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM system.columns WHERE database "
                             "= 'idx_test_ch_store_bin' AND table = 'swaps' AND "
                             "name = 'signature' AND compression_codec = "
                             "'CODEC(ZSTD(1))'"),
                1);

    /* An empty write set flushes to nothing rather than an empty insert. */
    idx_store_write_set empty;
    idx_store_write_set_init(&empty);
    TEST_EQ_INT(idx_finalized_store_append(bin, &empty, &err), IDX_OK);
    TEST_EQ_UINT(idx_ch_finalized_store_pending(bin), 0);
    TEST_EQ_INT(idx_finalized_store_flush(bin, &err), IDX_OK);

    TEST_EQ_STR(idx_finalized_store_name(bin), "clickhouse-finalized");

    idx_finalized_store_close(bin);
    idx_finalized_store_close(json);

    snprintf(sql, sizeof sql, "DROP DATABASE IF EXISTS %s", g_db_bin);
    TEST_EQ_INT(idx_ch_query(conn, sql, NULL, &err), IDX_OK);
    snprintf(sql, sizeof sql, "DROP DATABASE IF EXISTS %s", g_db_json);
    TEST_EQ_INT(idx_ch_query(conn, sql, NULL, &err), IDX_OK);
    idx_ch_close(conn);
}

TEST_MAIN({
    TEST_RUN(test_offline);
    if (getenv("IDX_TEST_CH_URL") != NULL) {
        TEST_RUN(test_online);
    } else {
        printf("  (skipping ch store online tests: set IDX_TEST_CH_URL)\n");
    }
})
