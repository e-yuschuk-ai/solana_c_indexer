/*
 * Venue swap decoding. The fixtures are the bytes each program actually emits,
 * built here from the layouts observed on mainnet, so a layout that drifts
 * fails here rather than silently naming the wrong account as a vault.
 *
 * Account `i` of the fixture transaction is the key whose every byte is `i`,
 * except the slots at the front, which hold the venue program ids so an
 * instruction selects its venue by index.
 */
#include "venue.h"

#include <string.h>

#include "test.h"
#include "venue_jupiter.h"
#include "venue_pump.h"
#include "venue_raydium.h"

#define ACCOUNT_COUNT 24
#define PROGRAM_PUMP_CURVE 0
#define PROGRAM_PUMP_AMM 1
#define PROGRAM_RAYDIUM_AMM 2
#define PROGRAM_RAYDIUM_CLMM 3
#define PROGRAM_JUPITER 4
#define PROGRAM_OTHER 5

typedef struct {
    idx_transaction tx;
    idx_account accounts[ACCOUNT_COUNT];
} fixture;

static void fixture_init(fixture *f) {
    memset(f, 0, sizeof(*f));
    for (size_t i = 0; i < ACCOUNT_COUNT; i++) {
        memset(f->accounts[i].pubkey.bytes, (int)i, IDX_PUBKEY_LEN);
    }
    f->accounts[PROGRAM_PUMP_CURVE].pubkey = IDX_PROGRAM_PUMP_CURVE;
    f->accounts[PROGRAM_PUMP_AMM].pubkey = IDX_PROGRAM_PUMP_AMM;
    f->accounts[PROGRAM_RAYDIUM_AMM].pubkey = IDX_PROGRAM_RAYDIUM_AMM_V4;
    f->accounts[PROGRAM_RAYDIUM_CLMM].pubkey = IDX_PROGRAM_RAYDIUM_CLMM;
    f->accounts[PROGRAM_JUPITER].pubkey = IDX_PROGRAM_JUPITER;
    f->tx.accounts = f->accounts;
    f->tx.account_count = ACCOUNT_COUNT;
}

static bool is_key(const idx_pubkey *key, uint8_t fill) {
    idx_pubkey expected;
    memset(expected.bytes, fill, IDX_PUBKEY_LEN);
    return idx_pubkey_equal(key, &expected);
}

/* ------------------------------------------------------------- payloads -- */

typedef struct {
    uint8_t bytes[512];
    size_t len;
} payload;

static void put_u8(payload *p, uint8_t value) {
    p->bytes[p->len++] = value;
}

static void put_u64(payload *p, uint64_t value) {
    for (size_t i = 0; i < 8; i++) {
        put_u8(p, (uint8_t)((value >> (8 * i)) & 0xff));
    }
}

static void put_bytes(payload *p, const uint8_t *bytes, size_t len) {
    memcpy(p->bytes + p->len, bytes, len);
    p->len += len;
}

/* A key whose every byte is `fill`, as the fixture accounts are. */
static void put_key(payload *p, uint8_t fill) {
    memset(p->bytes + p->len, fill, IDX_PUBKEY_LEN);
    p->len += IDX_PUBKEY_LEN;
}

/* Zero-fills up to `offset`, for the event fields between the ones read. */
static void pad_to(payload *p, size_t offset) {
    while (p->len < offset) {
        put_u8(p, 0);
    }
}

/* The Anchor self-invoke marker, then the event's own discriminator. */
static void put_event_header(payload *p, const uint8_t discriminator[8]) {
    put_bytes(p, IDX_ANCHOR_EVENT_DISCRIMINATOR, 8);
    put_bytes(p, discriminator, 8);
}

static const uint8_t TRADE_EVENT[8] = {0xbd, 0xdb, 0x7f, 0xd3,
                                       0x4e, 0xe6, 0x61, 0xee};
static const uint8_t BUY_EVENT[8] = {0x67, 0xf4, 0x52, 0x1f,
                                     0x2c, 0xf5, 0x77, 0x77};
static const uint8_t SELL_EVENT[8] = {0x3e, 0x2f, 0x37, 0x0a,
                                      0xa5, 0x03, 0xdc, 0x2a};
static const uint8_t SWAP_EVENT[8] = {0x40, 0xc6, 0xcd, 0xe8,
                                      0x26, 0x08, 0x71, 0xe2};
static const uint8_t CLMM_SWAP[8] = {0xf8, 0xc6, 0x9e, 0x91,
                                     0xe1, 0x75, 0x87, 0xc8};
static const uint8_t CLMM_SWAP_V2[8] = {0x2b, 0x04, 0xed, 0x0b,
                                        0x1a, 0xc9, 0x1e, 0x62};

/* Where the PumpSwap events keep what this decoder reads. */
#define AMM_BASE_AMOUNT 8
#define AMM_USER_QUOTE_AMOUNT 104
#define AMM_POOL 112
#define AMM_USER 144
#define AMM_USER_BASE_ACCOUNT 176
#define AMM_USER_QUOTE_ACCOUNT 208
#define AMM_PREFIX 240

static idx_instruction make_ix(uint8_t program, const uint8_t *indices,
                               size_t index_count, const payload *data) {
    idx_instruction ix;
    memset(&ix, 0, sizeof(ix));
    ix.program_id_index = program;
    ix.account_indices = indices;
    ix.account_count = index_count;
    ix.data = idx_slice_make(data->bytes, data->len);
    return ix;
}

static idx_status decode(const fixture *f, const idx_instruction *ix,
                         idx_swap *out) {
    idx_error err;
    idx_error_clear(&err);
    idx_status status = idx_swap_decode(&f->tx, ix, out, &err);
    if (status != IDX_OK) {
        TEST_CHECK(err.file != NULL, "failure recorded no context");
    }
    return status;
}

/* Eighteen accounts, which is more than any layout here needs. */
static const uint8_t INDICES[] = {6,  7,  8,  9,  10, 11, 12, 13, 14,
                                  15, 16, 17, 18, 19, 20, 21, 22, 23};

/* ---------------------------------------------------------------- tests -- */

/* The ids are bytes in the source; if one of them is wrong the decoder simply
 * never fires, which no other test would notice. */
static void test_program_ids(void) {
    const struct {
        const idx_pubkey *key;
        const char *expected;
    } ids[] = {
        {&IDX_PROGRAM_PUMP_CURVE,
         "6EF8rrecthR5Dkzon8Nwu78hRvfCKubJ14M5uBEwF6P"},
        {&IDX_PROGRAM_PUMP_AMM, "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"},
        {&IDX_PROGRAM_RAYDIUM_AMM_V4,
         "675kPX9MHTjS2zt1qfr1NYHuzeLXfQM9H24wFSUt1Mp8"},
        {&IDX_PROGRAM_RAYDIUM_CLMM,
         "CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTaW7grrKgrWqK"},
        {&IDX_PROGRAM_RAYDIUM_CPMM,
         "CPMMoo8L3F4NbTegBCKVNunggL7H1ZpdTHKxQB5qKP1C"},
        {&IDX_PROGRAM_JUPITER, "JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4"},
        {&IDX_MINT_WSOL, "So11111111111111111111111111111111111111112"},
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        char text[IDX_PUBKEY_STR_MAX];
        TEST_EQ_INT(idx_pubkey_to_base58(ids[i].key, text, NULL), IDX_OK);
        TEST_CHECK(strcmp(text, ids[i].expected) == 0,
                   "expected \"%s\", got \"%s\"", ids[i].expected, text);
    }

    TEST_EQ_INT(idx_venue_of_program(&IDX_PROGRAM_PUMP_AMM),
                IDX_VENUE_PUMP_AMM);
    TEST_EQ_INT(idx_venue_of_program(&IDX_PROGRAM_TOKEN), IDX_VENUE_NONE);
    TEST_EQ_INT(idx_venue_of_program(NULL), IDX_VENUE_NONE);
    TEST_EQ_STR(idx_venue_name(IDX_VENUE_RAYDIUM_CLMM), "raydium_clmm");
    TEST_EQ_STR(idx_venue_name((idx_venue)99), "unknown");

    /* An aggregator routes through pools and is not one. */
    TEST_ASSERT(idx_venue_is_pool(IDX_VENUE_PUMP_CURVE));
    TEST_ASSERT(idx_venue_is_pool(IDX_VENUE_RAYDIUM_AMM_V4));
    TEST_ASSERT(!idx_venue_is_pool(IDX_VENUE_JUPITER));
    TEST_ASSERT(!idx_venue_is_pool(IDX_VENUE_NONE));
}

/* The curve trades native SOL against the mint, and the event says which way
 * round. The SOL side is reported as the wrapped mint. */
static void test_pump_curve_trade_event(void) {
    fixture f;
    fixture_init(&f);
    idx_swap swap;

    payload p = {{0}, 0};
    put_event_header(&p, TRADE_EVENT);
    put_key(&p, 0x77);          /* mint */
    put_u64(&p, 494142551);     /* sol_amount */
    put_u64(&p, 4003969271065); /* token_amount */
    put_u8(&p, 1);              /* is_buy */
    put_key(&p, 0x21);          /* user */
    put_u64(&p, 1784679256);    /* the timestamp and tail this ignores */
    put_key(&p, 0xee);

    idx_instruction ix = make_ix(PROGRAM_PUMP_CURVE, INDICES, 1, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_OK);
    TEST_EQ_INT(swap.venue, IDX_VENUE_PUMP_CURVE);
    TEST_ASSERT(swap.from_event);
    TEST_ASSERT(swap.has_user && is_key(&swap.user, 0x21));
    TEST_ASSERT(swap.has_input_mint);
    TEST_ASSERT(idx_pubkey_equal(&swap.input_mint, &IDX_MINT_WSOL));
    TEST_EQ_UINT(swap.input_amount, 494142551);
    TEST_ASSERT(is_key(&swap.output_mint, 0x77));
    TEST_EQ_UINT(swap.output_amount, 4003969271065);
    /* The event does not name the curve; resolving it is the normalization
     * step's job. */
    TEST_ASSERT(!swap.has_pool);

    /* A sell is the same event with the sides the other way round. */
    p.bytes[16 + 48] = 0; /* is_buy */
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_OK);
    TEST_ASSERT(is_key(&swap.input_mint, 0x77));
    TEST_EQ_UINT(swap.input_amount, 4003969271065);
    TEST_ASSERT(idx_pubkey_equal(&swap.output_mint, &IDX_MINT_WSOL));
    TEST_EQ_UINT(swap.output_amount, 494142551);

    /* Cut short of the user, which is a truncated event rather than another
     * kind of instruction. */
    p.len = 16 + 60;
    ix = make_ix(PROGRAM_PUMP_CURVE, INDICES, 1, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_ERR_RANGE);
}

/* PumpSwap states base and quote; which is input and which output comes from
 * the event that carried them. */
static void test_pump_amm_events(void) {
    fixture f;
    fixture_init(&f);
    idx_swap swap;

    payload p = {{0}, 0};
    put_event_header(&p, BUY_EVENT);
    pad_to(&p, 16 + AMM_BASE_AMOUNT);
    put_u64(&p, 7387481318); /* base_amount_out */
    pad_to(&p, 16 + AMM_USER_QUOTE_AMOUNT);
    put_u64(&p, 196428569); /* user_quote_amount_in */
    pad_to(&p, 16 + AMM_POOL);
    put_key(&p, 0x30); /* pool */
    pad_to(&p, 16 + AMM_USER);
    put_key(&p, 0x21); /* user */
    pad_to(&p, 16 + AMM_USER_BASE_ACCOUNT);
    put_key(&p, 0x40); /* user_base_token_account */
    pad_to(&p, 16 + AMM_USER_QUOTE_ACCOUNT);
    put_key(&p, 0x41); /* user_quote_token_account */
    pad_to(&p, 16 + AMM_PREFIX);
    put_key(&p, 0xee); /* the tail this ignores */

    idx_instruction ix = make_ix(PROGRAM_PUMP_AMM, INDICES, 1, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_OK);
    TEST_EQ_INT(swap.venue, IDX_VENUE_PUMP_AMM);
    TEST_ASSERT(swap.has_pool && is_key(&swap.pool, 0x30));
    TEST_ASSERT(swap.has_user && is_key(&swap.user, 0x21));
    /* Buying the base means the quote went in. */
    TEST_ASSERT(is_key(&swap.input_account, 0x41));
    TEST_EQ_UINT(swap.input_amount, 196428569);
    TEST_ASSERT(is_key(&swap.output_account, 0x40));
    TEST_EQ_UINT(swap.output_amount, 7387481318);
    /* Neither event names a mint: base and quote belong to the pool. */
    TEST_ASSERT(!swap.has_input_mint && !swap.has_output_mint);

    /* The sell event has the same field order and the opposite direction. */
    memcpy(p.bytes + 8, SELL_EVENT, 8);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_OK);
    TEST_ASSERT(is_key(&swap.input_account, 0x40));
    TEST_EQ_UINT(swap.input_amount, 7387481318);
    TEST_ASSERT(is_key(&swap.output_account, 0x41));
    TEST_EQ_UINT(swap.output_amount, 196428569);

    /* One byte short of the last field this reads. */
    p.len = 16 + AMM_PREFIX - 1;
    ix = make_ix(PROGRAM_PUMP_AMM, INDICES, 1, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_ERR_RANGE);
}

/* Anything of these programs that is not a trade event is skipped, which is
 * most of what they do. */
static void test_pump_non_trades(void) {
    fixture f;
    fixture_init(&f);
    idx_swap swap;

    /* A plain instruction: `buy`, whose event is what gets decoded instead. */
    payload buy = {{0}, 0};
    put_bytes(&buy, (const uint8_t[]){0x66, 0x06, 0x3d, 0x12, 0x01, 0xda, 0xeb,
                                      0xea},
              8);
    put_u64(&buy, 1000);
    put_u64(&buy, 2000);
    idx_instruction ix = make_ix(PROGRAM_PUMP_CURVE, INDICES, 18, &buy);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_ERR_NOT_FOUND);

    /* An event of the same program that is not a trade. */
    payload other = {{0}, 0};
    put_event_header(&other, SWAP_EVENT); /* not one pump emits */
    put_key(&other, 0x11);
    ix = make_ix(PROGRAM_PUMP_AMM, INDICES, 1, &other);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_ERR_NOT_FOUND);
}

/* Raydium's AMM v4 states one amount and a limit; the other side is left for
 * the vault deltas, which is why the vaults are named. */
static void test_raydium_amm_v4(void) {
    fixture f;
    fixture_init(&f);
    idx_swap swap;

    payload p = {{0}, 0};
    put_u8(&p, 9);         /* SwapBaseIn */
    put_u64(&p, 78491305); /* amount_in */
    put_u64(&p, 0);        /* minimum_amount_out */
    idx_instruction ix = make_ix(PROGRAM_RAYDIUM_AMM, INDICES, 17, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_OK);
    TEST_EQ_INT(swap.venue, IDX_VENUE_RAYDIUM_AMM_V4);
    TEST_ASSERT(!swap.from_event);
    TEST_ASSERT(swap.has_pool && is_key(&swap.pool, INDICES[1]));
    TEST_ASSERT(swap.has_pool_accounts);
    TEST_ASSERT(is_key(&swap.pool_account_a, INDICES[4]));
    TEST_ASSERT(is_key(&swap.pool_account_b, INDICES[5]));
    /* The trader's three accounts sit at the end, whatever is in the middle. */
    TEST_ASSERT(is_key(&swap.input_account, INDICES[14]));
    TEST_ASSERT(is_key(&swap.output_account, INDICES[15]));
    TEST_ASSERT(swap.has_user && is_key(&swap.user, INDICES[16]));
    TEST_ASSERT(swap.has_input_amount && !swap.has_output_amount);
    TEST_EQ_UINT(swap.input_amount, 78491305);

    /* The eighteen-account shape pushes the vaults one along and moves the
     * trader's block with the end. */
    ix = make_ix(PROGRAM_RAYDIUM_AMM, INDICES, 18, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_OK);
    TEST_ASSERT(is_key(&swap.pool_account_a, INDICES[5]));
    TEST_ASSERT(is_key(&swap.pool_account_b, INDICES[6]));
    TEST_ASSERT(is_key(&swap.input_account, INDICES[15]));
    TEST_ASSERT(swap.has_user && is_key(&swap.user, INDICES[17]));

    /* SwapBaseOut fixes the other end. */
    payload out_form = {{0}, 0};
    put_u8(&out_form, 11);
    put_u64(&out_form, 999);  /* max_amount_in, a limit */
    put_u64(&out_form, 4242); /* amount_out */
    ix = make_ix(PROGRAM_RAYDIUM_AMM, INDICES, 17, &out_form);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_OK);
    TEST_ASSERT(!swap.has_input_amount && swap.has_output_amount);
    TEST_EQ_UINT(swap.output_amount, 4242);

    /* A layout this decoder has not seen is skipped rather than guessed at:
     * naming the wrong account as a vault would attribute someone else's
     * delta to this pool. */
    ix = make_ix(PROGRAM_RAYDIUM_AMM, INDICES, 16, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_ERR_NOT_FOUND);

    /* And a non-swap instruction of the same program. */
    payload deposit = {{0}, 0};
    put_u8(&deposit, 3);
    put_u64(&deposit, 1);
    put_u64(&deposit, 2);
    ix = make_ix(PROGRAM_RAYDIUM_AMM, INDICES, 17, &deposit);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_ERR_NOT_FOUND);
}

/* CLMM carries the direction in the payload rather than in the discriminant,
 * and swapV2 names the two mints outright. */
static void test_raydium_clmm(void) {
    fixture f;
    fixture_init(&f);
    idx_swap swap;

    payload p = {{0}, 0};
    put_bytes(&p, CLMM_SWAP_V2, 8);
    put_u64(&p, 3061218841); /* amount */
    put_u64(&p, 1);          /* other_amount_threshold */
    for (size_t i = 0; i < 16; i++) {
        put_u8(&p, 0); /* sqrt_price_limit_x64 */
    }
    put_u8(&p, 1); /* is_base_input */

    idx_instruction ix = make_ix(PROGRAM_RAYDIUM_CLMM, INDICES, 17, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_OK);
    TEST_EQ_INT(swap.venue, IDX_VENUE_RAYDIUM_CLMM);
    TEST_ASSERT(swap.has_pool && is_key(&swap.pool, INDICES[2]));
    TEST_ASSERT(swap.has_user && is_key(&swap.user, INDICES[0]));
    TEST_ASSERT(is_key(&swap.input_account, INDICES[3]));
    TEST_ASSERT(is_key(&swap.output_account, INDICES[4]));
    TEST_ASSERT(is_key(&swap.pool_account_a, INDICES[5]));
    TEST_ASSERT(is_key(&swap.pool_account_b, INDICES[6]));
    TEST_ASSERT(swap.has_input_amount && !swap.has_output_amount);
    TEST_EQ_UINT(swap.input_amount, 3061218841);
    TEST_ASSERT(swap.has_input_mint && is_key(&swap.input_mint, INDICES[11]));
    TEST_ASSERT(swap.has_output_mint && is_key(&swap.output_mint, INDICES[12]));

    /* is_base_input false makes the stated amount the output. */
    p.bytes[8 + 32] = 0;
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_OK);
    TEST_ASSERT(!swap.has_input_amount && swap.has_output_amount);
    TEST_EQ_UINT(swap.output_amount, 3061218841);

    /* The v1 swap has the same leading accounts and names no mints. */
    payload v1 = {{0}, 0};
    put_bytes(&v1, CLMM_SWAP, 8);
    put_u64(&v1, 500);
    put_u64(&v1, 1);
    for (size_t i = 0; i < 16; i++) {
        put_u8(&v1, 0);
    }
    put_u8(&v1, 1);
    ix = make_ix(PROGRAM_RAYDIUM_CLMM, INDICES, 10, &v1);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_OK);
    TEST_ASSERT(is_key(&swap.pool, INDICES[2]));
    TEST_EQ_UINT(swap.input_amount, 500);
    TEST_ASSERT(!swap.has_input_mint && !swap.has_output_mint);

    /* swapV2 over too few accounts to hold the mints is skipped. */
    ix = make_ix(PROGRAM_RAYDIUM_CLMM, INDICES, 10, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_ERR_NOT_FOUND);
}

/* Jupiter states the mints and the exact amounts of one leg, which is what
 * makes a leg through a venue with no decoder visible at all. */
static void test_jupiter_swap_event(void) {
    fixture f;
    fixture_init(&f);
    idx_swap swap;

    payload p = {{0}, 0};
    put_event_header(&p, SWAP_EVENT);
    put_key(&p, 0x01);            /* amm, which this drops */
    put_key(&p, 0x77);            /* input_mint */
    put_u64(&p, 634524165458);    /* input_amount */
    put_key(&p, 0x78);            /* output_mint */
    put_u64(&p, 2811151606);      /* output_amount */

    idx_instruction ix = make_ix(PROGRAM_JUPITER, INDICES, 1, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_OK);
    TEST_EQ_INT(swap.venue, IDX_VENUE_JUPITER);
    TEST_ASSERT(swap.from_event);
    TEST_ASSERT(is_key(&swap.input_mint, 0x77));
    TEST_EQ_UINT(swap.input_amount, 634524165458);
    TEST_ASSERT(is_key(&swap.output_mint, 0x78));
    TEST_EQ_UINT(swap.output_amount, 2811151606);
    /* The event's `amm` is not reliably a pool, so no pool is claimed. */
    TEST_ASSERT(!swap.has_pool);

    /* A route instruction produces nothing: its legs are what count. */
    payload route = {{0}, 0};
    put_bytes(&route, (const uint8_t[]){0xe5, 0x17, 0xcb, 0x97, 0x7a, 0xe3,
                                        0xad, 0x2a},
              8);
    put_u64(&route, 1);
    ix = make_ix(PROGRAM_JUPITER, INDICES, 18, &route);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_ERR_NOT_FOUND);

    /* Truncated in the middle of the output mint. */
    p.len = 16 + 100;
    ix = make_ix(PROGRAM_JUPITER, INDICES, 1, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_ERR_RANGE);
}

static void test_dispatch(void) {
    fixture f;
    fixture_init(&f);
    idx_swap swap;

    payload p = {{0}, 0};
    put_u8(&p, 9);
    put_u64(&p, 1);
    put_u64(&p, 2);
    idx_instruction ix = make_ix(PROGRAM_OTHER, INDICES, 17, &p);
    TEST_EQ_INT(decode(&f, &ix, &swap), IDX_ERR_NOT_FOUND);

    TEST_EQ_INT(idx_swap_decode(NULL, &ix, &swap, NULL), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_swap_decode(&f.tx, NULL, &swap, NULL), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_swap_decode(&f.tx, &ix, NULL, NULL), IDX_ERR_INVALID_ARG);
}

static void test_anchor_event_helpers(void) {
    payload p = {{0}, 0};
    put_event_header(&p, SWAP_EVENT);
    put_u64(&p, 7);

    idx_slice payload_slice;
    TEST_ASSERT(idx_anchor_event_payload(idx_slice_make(p.bytes, p.len),
                                         &payload_slice));
    TEST_EQ_UINT(payload_slice.len, p.len - 8);

    idx_slice fields;
    TEST_ASSERT(idx_anchor_event_is(payload_slice, SWAP_EVENT, &fields));
    TEST_EQ_UINT(fields.len, 8);
    TEST_ASSERT(!idx_anchor_event_is(payload_slice, TRADE_EVENT, &fields));

    /* Too short to carry a marker at all. */
    TEST_ASSERT(!idx_anchor_event_payload(idx_slice_make(p.bytes, 4), NULL));
    TEST_ASSERT(!idx_anchor_event_payload(idx_slice_make(NULL, 0), NULL));
}

TEST_MAIN({
    TEST_RUN(test_program_ids);
    TEST_RUN(test_pump_curve_trade_event);
    TEST_RUN(test_pump_amm_events);
    TEST_RUN(test_pump_non_trades);
    TEST_RUN(test_raydium_amm_v4);
    TEST_RUN(test_raydium_clmm);
    TEST_RUN(test_jupiter_swap_event);
    TEST_RUN(test_dispatch);
    TEST_RUN(test_anchor_event_helpers);
})
