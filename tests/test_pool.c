/*
 * Pool registry. Two halves, matching the two sources D5 names: the registry
 * itself, fed swap rows directly (the structure a swap reveals), and the
 * pump.fun CreateEvent decoder (the enrichment a creation adds). The swap
 * normalizer is exercised in test_swap; here rows are built by hand so the
 * registry's own rules — insert-once, match-mints-by-identity, enrich-only — are
 * what is under test.
 */
#include "pool.h"

#include <string.h>

#include "test.h"
#include "venue.h"
#include "venue_pump.h"

static idx_pubkey key_fill(uint8_t fill) {
    idx_pubkey k;
    memset(k.bytes, fill, IDX_PUBKEY_LEN);
    return k;
}

static idx_swap_row pool_row(idx_venue venue, idx_pubkey pool,
                             idx_pubkey in_mint, uint8_t in_dec,
                             idx_pubkey out_mint, uint8_t out_dec) {
    idx_swap_row row;
    memset(&row, 0, sizeof(row));
    row.kind = IDX_SWAP_POOL;
    row.venue = venue;
    row.pool = pool;
    row.has_pool = true;
    row.input_mint = in_mint;
    row.has_input_mint = true;
    row.input_decimals = in_dec;
    row.has_input_decimals = true;
    row.output_mint = out_mint;
    row.has_output_mint = true;
    row.output_decimals = out_dec;
    row.has_output_decimals = true;
    return row;
}

static idx_status observe(idx_pool_registry *reg, const idx_swap_row *row,
                          idx_slot slot) {
    idx_error err;
    idx_error_clear(&err);
    return idx_pool_registry_observe_swap(reg, row, slot, &err);
}

/* ------------------------------------------------------------- registry -- */

/* The first swap creates the record with its structure; the pool address, venue
 * and both mints and decimals come straight off the row. */
static void test_first_swap_registers(void) {
    idx_pool_registry reg;
    idx_pool_registry_init(&reg);

    idx_pubkey pool = key_fill(0x30);
    idx_swap_row row = pool_row(IDX_VENUE_RAYDIUM_AMM_V4, pool,
                                IDX_MINT_WSOL, 9, key_fill(0x25), 6);
    TEST_EQ_INT(observe(&reg, &row, 100), IDX_OK);

    TEST_EQ_UINT(idx_pool_registry_count(&reg), 1);
    const idx_pool *p = idx_pool_registry_get(&reg, &pool);
    TEST_ASSERT(p != NULL);
    TEST_EQ_INT(p->venue, IDX_VENUE_RAYDIUM_AMM_V4);
    TEST_EQ_UINT(p->first_seen_slot, 100);
    TEST_EQ_UINT(p->swap_count, 1);
    TEST_ASSERT(p->has_mint_a && p->has_mint_b);
    TEST_ASSERT(p->has_decimals_a && p->has_decimals_b);
    TEST_ASSERT(!p->has_creation);

    idx_pool_registry_free(&reg);
}

/* A second swap of the same pool does not add a record, and a sell — mints in
 * the opposite order — matches the same two slots by identity rather than
 * creating a mirror pair. */
static void test_repeat_swap_matches_by_identity(void) {
    idx_pool_registry reg;
    idx_pool_registry_init(&reg);

    idx_pubkey pool = key_fill(0x30);
    idx_pubkey wsol = IDX_MINT_WSOL;
    idx_pubkey token = key_fill(0x25);

    idx_swap_row buy = pool_row(IDX_VENUE_RAYDIUM_AMM_V4, pool, wsol, 9, token, 6);
    idx_swap_row sell = pool_row(IDX_VENUE_RAYDIUM_AMM_V4, pool, token, 6, wsol, 9);
    TEST_EQ_INT(observe(&reg, &buy, 100), IDX_OK);
    TEST_EQ_INT(observe(&reg, &sell, 101), IDX_OK);

    TEST_EQ_UINT(idx_pool_registry_count(&reg), 1);
    const idx_pool *p = idx_pool_registry_get(&reg, &pool);
    TEST_ASSERT(p != NULL);
    TEST_EQ_UINT(p->swap_count, 2);
    TEST_EQ_UINT(p->first_seen_slot, 100); /* the first, not the last */
    /* The pair is still exactly {wsol, token}, in first-seen order. */
    TEST_ASSERT(idx_pubkey_equal(&p->mint_a, &wsol));
    TEST_ASSERT(idx_pubkey_equal(&p->mint_b, &token));

    idx_pool_registry_free(&reg);
}

/* Decimals a first swap could not resolve are filled by a later one that could,
 * matched to the right mint slot. */
static void test_decimals_filled_later(void) {
    idx_pool_registry reg;
    idx_pool_registry_init(&reg);

    idx_pubkey pool = key_fill(0x30);
    idx_pubkey token = key_fill(0x25);

    idx_swap_row first = pool_row(IDX_VENUE_RAYDIUM_CLMM, pool, IDX_MINT_WSOL, 9,
                                  token, 6);
    first.has_output_decimals = false; /* the token scale was unknown */
    TEST_EQ_INT(observe(&reg, &first, 100), IDX_OK);
    const idx_pool *p = idx_pool_registry_get(&reg, &pool);
    TEST_ASSERT(p->has_mint_b && !p->has_decimals_b);

    idx_swap_row second = pool_row(IDX_VENUE_RAYDIUM_CLMM, pool, IDX_MINT_WSOL, 9,
                                   token, 6); /* now with decimals */
    TEST_EQ_INT(observe(&reg, &second, 101), IDX_OK);
    p = idx_pool_registry_get(&reg, &pool);
    TEST_ASSERT(p->has_decimals_b && p->decimals_b == 6);

    idx_pool_registry_free(&reg);
}

/* A route row and a pool-less row register nothing. */
static void test_ignored_rows(void) {
    idx_pool_registry reg;
    idx_pool_registry_init(&reg);

    idx_swap_row route = pool_row(IDX_VENUE_JUPITER, key_fill(0x30),
                                  key_fill(0x25), 6, IDX_MINT_WSOL, 9);
    route.kind = IDX_SWAP_AGGREGATED;
    TEST_EQ_INT(observe(&reg, &route, 100), IDX_OK);

    idx_swap_row no_pool = pool_row(IDX_VENUE_RAYDIUM_AMM_V4, key_fill(0x30),
                                    key_fill(0x25), 6, IDX_MINT_WSOL, 9);
    no_pool.has_pool = false;
    TEST_EQ_INT(observe(&reg, &no_pool, 100), IDX_OK);

    TEST_EQ_UINT(idx_pool_registry_count(&reg), 0);
    idx_pool_registry_free(&reg);
}

/* ------------------------------------------------------------ creation -- */

static idx_status observe_creation(idx_pool_registry *reg,
                                   const idx_pool_creation *c, idx_slot slot) {
    idx_error err;
    idx_error_clear(&err);
    return idx_pool_registry_observe_creation(reg, c, slot, &err);
}

/* A creation enriches a pool that a swap already registered, but never creates a
 * record of its own (D5): a creation for an untraded pool is dropped and
 * counted. */
static void test_creation_enriches_existing_only(void) {
    idx_pool_registry reg;
    idx_pool_registry_init(&reg);

    idx_pubkey pool = key_fill(0x30);
    idx_pubkey creator = key_fill(0x31);

    idx_pool_creation orphan;
    memset(&orphan, 0, sizeof(orphan));
    orphan.venue = IDX_VENUE_PUMP_CURVE;
    orphan.pool = pool;
    orphan.has_pool = true;
    orphan.creator = creator;
    orphan.has_creator = true;
    TEST_EQ_INT(observe_creation(&reg, &orphan, 50), IDX_OK);
    TEST_EQ_UINT(idx_pool_registry_count(&reg), 0); /* no record made */
    TEST_EQ_UINT(reg.creations_unmatched, 1);
    TEST_EQ_UINT(reg.enriched, 0);

    /* Now the pool trades, then the same creation is seen: it enriches. */
    idx_swap_row row = pool_row(IDX_VENUE_PUMP_CURVE, pool, IDX_MINT_WSOL, 9,
                                key_fill(0x25), 6);
    TEST_EQ_INT(observe(&reg, &row, 100), IDX_OK);
    TEST_EQ_INT(observe_creation(&reg, &orphan, 100), IDX_OK);

    const idx_pool *p = idx_pool_registry_get(&reg, &pool);
    TEST_ASSERT(p->has_creation && p->creation_slot == 100);
    TEST_ASSERT(p->has_creator && idx_pubkey_equal(&p->creator, &creator));
    TEST_EQ_UINT(reg.enriched, 1);

    /* A second creation is idempotent: the first one seen stands. */
    idx_pool_creation again = orphan;
    again.creator = key_fill(0x77);
    TEST_EQ_INT(observe_creation(&reg, &again, 200), IDX_OK);
    p = idx_pool_registry_get(&reg, &pool);
    TEST_EQ_UINT(p->creation_slot, 100);
    TEST_ASSERT(idx_pubkey_equal(&p->creator, &creator));
    TEST_EQ_UINT(reg.enriched, 1);

    idx_pool_registry_free(&reg);
}

/* -------------------------------------------------- pump CreateEvent -- */

typedef struct {
    uint8_t bytes[256];
    size_t len;
} buf;

static void put_bytes(buf *b, const uint8_t *data, size_t n) {
    memcpy(b->bytes + b->len, data, n);
    b->len += n;
}
static void put_u32le(buf *b, uint32_t v) {
    for (size_t i = 0; i < 4; i++) {
        b->bytes[b->len++] = (uint8_t)((v >> (8 * i)) & 0xff);
    }
}
static void put_key(buf *b, uint8_t fill) {
    memset(b->bytes + b->len, fill, IDX_PUBKEY_LEN);
    b->len += IDX_PUBKEY_LEN;
}
static void put_string(buf *b, const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    put_u32le(b, n);
    put_bytes(b, (const uint8_t *)s, n);
}

/* The CreateEvent names the mint (the curve's identity) and the user (its
 * creator); the three leading strings and the bonding-curve account are read
 * past, not stored. */
static void test_pump_create_event_decode(void) {
    static const uint8_t ANCHOR_EVENT[8] = {0xe4, 0x45, 0xa5, 0x2e,
                                            0x51, 0xcb, 0x9a, 0x1d};
    static const uint8_t CREATE_EVENT[8] = {0x1b, 0x72, 0xa9, 0x4d,
                                            0xde, 0xeb, 0x63, 0x76};
    buf b;
    b.len = 0;
    put_bytes(&b, ANCHOR_EVENT, 8);
    put_bytes(&b, CREATE_EVENT, 8);
    put_string(&b, "My Token");
    put_string(&b, "MYT");
    put_string(&b, "https://example.com/meta.json");
    put_key(&b, 0x25); /* mint */
    put_key(&b, 0x40); /* bonding_curve */
    put_key(&b, 0x31); /* user = creator */

    idx_account accounts[1];
    memset(accounts, 0, sizeof(accounts));
    accounts[0].pubkey = IDX_PROGRAM_PUMP_CURVE;

    idx_instruction ix;
    memset(&ix, 0, sizeof(ix));
    ix.program_id_index = 0;
    ix.data = idx_slice_make(b.bytes, b.len);

    idx_transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.accounts = accounts;
    tx.account_count = 1;

    idx_pool_creation c;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_venue_creation_decode(&tx, &ix, &c, &err), IDX_OK);
    TEST_EQ_INT(c.venue, IDX_VENUE_PUMP_CURVE);
    TEST_ASSERT(c.has_pool);
    idx_pubkey expect_mint = key_fill(0x25);
    idx_pubkey expect_user = key_fill(0x31);
    TEST_ASSERT(idx_pubkey_equal(&c.pool, &expect_mint));
    TEST_ASSERT(c.has_creator && idx_pubkey_equal(&c.creator, &expect_user));

    /* A truncated event — missing the user pubkey — is a range error, not a
     * bad record. */
    ix.data = idx_slice_make(b.bytes, b.len - 10);
    TEST_EQ_INT(idx_venue_creation_decode(&tx, &ix, &c, &err), IDX_ERR_RANGE);

    idx_error_clear(&err);
}

/* A non-creation instruction is not-found, the common answer. */
static void test_creation_not_found(void) {
    idx_account accounts[1];
    memset(accounts, 0, sizeof(accounts));
    accounts[0].pubkey = IDX_PROGRAM_PUMP_CURVE;
    uint8_t data[8] = {0}; /* not the anchor event marker */
    idx_instruction ix;
    memset(&ix, 0, sizeof(ix));
    ix.data = idx_slice_make(data, sizeof(data));
    idx_transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.accounts = accounts;
    tx.account_count = 1;

    idx_pool_creation c;
    TEST_EQ_INT(idx_venue_creation_decode(&tx, &ix, &c, NULL), IDX_ERR_NOT_FOUND);
}

TEST_MAIN({
    TEST_RUN(test_first_swap_registers);
    TEST_RUN(test_repeat_swap_matches_by_identity);
    TEST_RUN(test_decimals_filled_later);
    TEST_RUN(test_ignored_rows);
    TEST_RUN(test_creation_enriches_existing_only);
    TEST_RUN(test_pump_create_event_decode);
    TEST_RUN(test_creation_not_found);
})
