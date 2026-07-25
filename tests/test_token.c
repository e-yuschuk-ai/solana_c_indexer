/*
 * Token registry. Two sources, as D5 splits them: the balance path (mint and
 * decimals, free from every token balance) and the metadata path (name, symbol
 * and URI, only when a metadata instruction is seen). The pump CreateEvent
 * decode is exercised through idx_venue_metadata_decode against a hand-built
 * event whose byte layout matches the mainnet one verified while building this.
 */
#include "token.h"

#include <string.h>

#include "test.h"
#include "venue.h"
#include "venue_pump.h"

static idx_pubkey key_fill(uint8_t fill) {
    idx_pubkey k;
    memset(k.bytes, fill, IDX_PUBKEY_LEN);
    return k;
}

static idx_status obs_balance(idx_token_registry *reg, const idx_pubkey *mint,
                              uint8_t dec, idx_slot slot) {
    idx_error err;
    idx_error_clear(&err);
    return idx_token_registry_observe_balance(reg, mint, dec, slot, &err);
}

static idx_status obs_meta(idx_token_registry *reg, const idx_token_metadata *m,
                           idx_slot slot) {
    idx_error err;
    idx_error_clear(&err);
    return idx_token_registry_observe_metadata(reg, m, slot, &err);
}

/* A balance registers a token with its decimals and first-seen slot. */
static void test_balance_registers(void) {
    idx_token_registry reg;
    idx_token_registry_init(&reg);
    idx_pubkey mint = key_fill(0x25);

    TEST_EQ_INT(obs_balance(&reg, &mint, 6, 100), IDX_OK);
    TEST_EQ_INT(obs_balance(&reg, &mint, 6, 101), IDX_OK); /* second, same mint */
    TEST_EQ_UINT(idx_token_registry_count(&reg), 1);

    const idx_token *t = idx_token_registry_get(&reg, &mint);
    TEST_ASSERT(t != NULL);
    TEST_ASSERT(t->has_decimals && t->decimals == 6);
    TEST_EQ_UINT(t->first_seen_slot, 100);
    TEST_ASSERT(!t->has_metadata);

    idx_token_registry_free(&reg);
}

/* Metadata copies name/symbol/uri, may create the record, and the first one
 * seen stands against a later overwrite. */
static void test_metadata_enriches(void) {
    idx_token_registry reg;
    idx_token_registry_init(&reg);
    idx_pubkey mint = key_fill(0x25);

    idx_token_metadata m;
    memset(&m, 0, sizeof(m));
    m.mint = mint;
    m.has_mint = true;
    m.name = idx_slice_from_str("My Token");
    m.has_name = true;
    m.symbol = idx_slice_from_str("MYT");
    m.has_symbol = true;
    m.uri = idx_slice_from_str("https://example.com/m.json");
    m.has_uri = true;

    /* Metadata for an untraded mint still registers it (a token is a token). */
    TEST_EQ_INT(obs_meta(&reg, &m, 50), IDX_OK);
    TEST_EQ_UINT(idx_token_registry_count(&reg), 1);
    TEST_EQ_UINT(reg.with_metadata, 1);

    const idx_token *t = idx_token_registry_get(&reg, &mint);
    TEST_ASSERT(t->has_name && strcmp(t->name, "My Token") == 0);
    TEST_ASSERT(t->has_symbol && strcmp(t->symbol, "MYT") == 0);
    TEST_ASSERT(t->has_uri && strcmp(t->uri, "https://example.com/m.json") == 0);
    TEST_ASSERT(!t->has_decimals); /* no balance seen yet */

    /* A balance now fills the decimals without disturbing the metadata. */
    TEST_EQ_INT(obs_balance(&reg, &mint, 9, 60), IDX_OK);
    t = idx_token_registry_get(&reg, &mint);
    TEST_ASSERT(t->has_decimals && t->decimals == 9);

    /* A second metadata does not overwrite the first. */
    idx_token_metadata m2 = m;
    m2.name = idx_slice_from_str("Renamed");
    TEST_EQ_INT(obs_meta(&reg, &m2, 70), IDX_OK);
    t = idx_token_registry_get(&reg, &mint);
    TEST_ASSERT(strcmp(t->name, "My Token") == 0);
    TEST_EQ_UINT(reg.with_metadata, 1);

    idx_token_registry_free(&reg);
}

/* An over-long name is truncated to the buffer rather than overflowing. */
static void test_metadata_truncates(void) {
    idx_token_registry reg;
    idx_token_registry_init(&reg);
    idx_pubkey mint = key_fill(0x25);

    char big[128];
    memset(big, 'x', sizeof(big));
    idx_token_metadata m;
    memset(&m, 0, sizeof(m));
    m.mint = mint;
    m.has_mint = true;
    m.name = idx_slice_make(big, sizeof(big));
    m.has_name = true;

    TEST_EQ_INT(obs_meta(&reg, &m, 50), IDX_OK);
    const idx_token *t = idx_token_registry_get(&reg, &mint);
    TEST_ASSERT(t->has_name);
    TEST_EQ_UINT(strlen(t->name), IDX_TOKEN_NAME_MAX - 1);

    idx_token_registry_free(&reg);
}

/* Metadata that names no mint is a no-op. */
static void test_metadata_no_mint(void) {
    idx_token_registry reg;
    idx_token_registry_init(&reg);
    idx_token_metadata m;
    memset(&m, 0, sizeof(m));
    m.name = idx_slice_from_str("orphan");
    m.has_name = true;
    TEST_EQ_INT(obs_meta(&reg, &m, 50), IDX_OK);
    TEST_EQ_UINT(idx_token_registry_count(&reg), 0);
    idx_token_registry_free(&reg);
}

/* ---------------------------------------------- pump CreateEvent metadata -- */

typedef struct {
    uint8_t bytes[256];
    size_t len;
} buf;
static void put_bytes(buf *b, const uint8_t *d, size_t n) {
    memcpy(b->bytes + b->len, d, n);
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

/* idx_venue_metadata_decode pulls name/symbol/uri and the mint out of a pump
 * CreateEvent — the same event the pool creator is read from. */
static void test_pump_metadata_decode(void) {
    static const uint8_t ANCHOR_EVENT[8] = {0xe4, 0x45, 0xa5, 0x2e,
                                            0x51, 0xcb, 0x9a, 0x1d};
    static const uint8_t CREATE_EVENT[8] = {0x1b, 0x72, 0xa9, 0x4d,
                                            0xde, 0xeb, 0x63, 0x76};
    buf b;
    b.len = 0;
    put_bytes(&b, ANCHOR_EVENT, 8);
    put_bytes(&b, CREATE_EVENT, 8);
    put_string(&b, "United States Water Reserve");
    put_string(&b, "USWR");
    put_string(&b, "https://ipfs.io/ipfs/Qm.../meta.json");
    put_key(&b, 0x25); /* mint */
    put_key(&b, 0x40); /* bonding_curve */
    put_key(&b, 0x31); /* user */

    idx_account accounts[1];
    memset(accounts, 0, sizeof(accounts));
    accounts[0].pubkey = IDX_PROGRAM_PUMP_CURVE;
    idx_instruction ix;
    memset(&ix, 0, sizeof(ix));
    ix.data = idx_slice_make(b.bytes, b.len);
    idx_transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.accounts = accounts;
    tx.account_count = 1;

    idx_token_metadata m;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_venue_metadata_decode(&tx, &ix, &m, &err), IDX_OK);
    TEST_ASSERT(m.has_mint);
    idx_pubkey expect = key_fill(0x25);
    TEST_ASSERT(idx_pubkey_equal(&m.mint, &expect));
    TEST_ASSERT(m.has_name && idx_slice_equal(
                                  m.name, idx_slice_from_str(
                                              "United States Water Reserve")));
    TEST_ASSERT(m.has_symbol &&
                idx_slice_equal(m.symbol, idx_slice_from_str("USWR")));
    TEST_ASSERT(m.has_uri);

    /* And it flows into the registry. */
    idx_token_registry reg;
    idx_token_registry_init(&reg);
    TEST_EQ_INT(obs_meta(&reg, &m, 100), IDX_OK);
    const idx_token *t = idx_token_registry_get(&reg, &expect);
    TEST_ASSERT(t != NULL && strcmp(t->symbol, "USWR") == 0);
    idx_token_registry_free(&reg);
}

TEST_MAIN({
    TEST_RUN(test_balance_registers);
    TEST_RUN(test_metadata_enriches);
    TEST_RUN(test_metadata_truncates);
    TEST_RUN(test_metadata_no_mint);
    TEST_RUN(test_pump_metadata_decode);
})
