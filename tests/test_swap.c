/*
 * Swap normalization. The venue decoders are tested for their bytes in
 * test_venue; here the fixtures are whole transactions — instruction, event
 * and the token balances and logs of `meta` — because normalization's job is
 * the join between them: the mint an event omits, the amount an instruction
 * leaves to a log or a vault delta, and the route netted to its endpoints.
 *
 * Account slots at the front hold the venue program ids; every other account
 * `i` is the key whose bytes are all `i`. Account 26 is set to WSOL so a quote
 * side can be the wrapped mint.
 */
#include "swap.h"

#include <stdio.h>
#include <string.h>

#include "base64.h"
#include "test.h"
#include "venue.h"
#include "venue_pump.h"
#include "venue_raydium.h"

#define ACCOUNT_COUNT 40
#define PROGRAM_PUMP_CURVE 0
#define PROGRAM_PUMP_AMM 1
#define PROGRAM_RAYDIUM_AMM 2
#define PROGRAM_RAYDIUM_CLMM 3
#define PROGRAM_JUPITER 4
#define ACCOUNT_WSOL 26

#define MAX_IX 8
#define MAX_INNER 16
#define MAX_TB 16
#define MAX_LOGS 8
#define MAX_PAYLOADS 24

typedef struct {
    uint8_t bytes[512];
    size_t len;
} payload;

typedef struct {
    idx_transaction tx;
    idx_account accounts[ACCOUNT_COUNT];
    idx_instruction top[MAX_IX];
    idx_instruction inner[MAX_INNER];
    size_t inner_used;
    idx_inner_instructions groups[MAX_IX];
    idx_token_balance pre[MAX_TB];
    size_t pre_count;
    idx_token_balance post[MAX_TB];
    size_t post_count;
    idx_slice logs[MAX_LOGS];
    char logbuf[MAX_LOGS][256];
    size_t log_count;
    payload payloads[MAX_PAYLOADS];
    size_t payload_count;
    idx_arena arena;
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
    f->accounts[ACCOUNT_WSOL].pubkey = IDX_MINT_WSOL;
    f->tx.accounts = f->accounts;
    f->tx.account_count = ACCOUNT_COUNT;
    f->tx.instructions = f->top;
    f->tx.inner_instructions = f->groups;
    f->tx.has_meta = true;
    f->tx.success = true;
    idx_arena_init(&f->arena, 0);
}

static void fixture_free(fixture *f) {
    idx_arena_destroy(&f->arena);
}

static bool is_key(const idx_pubkey *key, uint8_t fill) {
    idx_pubkey expected;
    memset(expected.bytes, fill, IDX_PUBKEY_LEN);
    return idx_pubkey_equal(key, &expected);
}

/* ------------------------------------------------------------- builders -- */

static payload *new_payload(fixture *f) {
    payload *p = &f->payloads[f->payload_count++];
    p->len = 0;
    return p;
}

static void put_u8(payload *p, uint8_t v) { p->bytes[p->len++] = v; }
static void put_u64(payload *p, uint64_t v) {
    for (size_t i = 0; i < 8; i++) {
        put_u8(p, (uint8_t)((v >> (8 * i)) & 0xff));
    }
}
static void put_bytes(payload *p, const uint8_t *b, size_t n) {
    memcpy(p->bytes + p->len, b, n);
    p->len += n;
}
static void put_key(payload *p, uint8_t fill) {
    memset(p->bytes + p->len, fill, IDX_PUBKEY_LEN);
    p->len += IDX_PUBKEY_LEN;
}
static void put_at(payload *p, size_t off, const void *b, size_t n) {
    memcpy(p->bytes + off, b, n);
    if (off + n > p->len) {
        p->len = off + n;
    }
}
static void put_key_at(payload *p, size_t off, uint8_t fill) {
    uint8_t key[IDX_PUBKEY_LEN];
    memset(key, fill, IDX_PUBKEY_LEN);
    put_at(p, off, key, IDX_PUBKEY_LEN);
}
static void put_u64_at(payload *p, size_t off, uint64_t v) {
    uint8_t b[8];
    for (size_t i = 0; i < 8; i++) {
        b[i] = (uint8_t)((v >> (8 * i)) & 0xff);
    }
    put_at(p, off, b, 8);
}

static payload *set_top(fixture *f, size_t slot, uint8_t program,
                        const uint8_t *indices, size_t index_count) {
    payload *p = new_payload(f);
    idx_instruction *ix = &f->top[slot];
    memset(ix, 0, sizeof(*ix));
    ix->program_id_index = program;
    ix->account_indices = indices;
    ix->account_count = index_count;
    ix->data = idx_slice_make(p->bytes, 0);
    if (slot + 1 > f->tx.instruction_count) {
        f->tx.instruction_count = slot + 1;
    }
    return p;
}

/* Appends an inner instruction under top-level `parent`; the inner slots of one
 * group are consecutive because a group's instructions are added together. */
static idx_instruction *add_inner(fixture *f, size_t parent, uint8_t program,
                                  const uint8_t *indices, size_t index_count,
                                  payload **out_payload) {
    idx_inner_instructions *group = NULL;
    for (size_t i = 0; i < f->tx.inner_instruction_count; i++) {
        if (f->groups[i].index == parent) {
            group = &f->groups[i];
            break;
        }
    }
    if (group == NULL) {
        group = &f->groups[f->tx.inner_instruction_count++];
        group->index = (uint8_t)parent;
        group->instructions = &f->inner[f->inner_used];
        group->instruction_count = 0;
    }
    idx_instruction *ix = &f->inner[f->inner_used++];
    group->instruction_count++;
    payload *p = new_payload(f);
    memset(ix, 0, sizeof(*ix));
    ix->program_id_index = program;
    ix->account_indices = indices;
    ix->account_count = index_count;
    ix->data = idx_slice_make(p->bytes, 0);
    *out_payload = p;
    return ix;
}

static void seal(idx_instruction *ix, const payload *p) {
    ix->data = idx_slice_make(p->bytes, p->len);
}

static void add_balance(idx_token_balance *list, size_t *count, uint8_t index,
                        uint8_t mint_fill, uint8_t decimals, uint64_t amount) {
    idx_token_balance *e = &list[(*count)++];
    memset(e, 0, sizeof(*e));
    e->account_index = index;
    memset(e->mint.bytes, mint_fill, IDX_PUBKEY_LEN);
    e->decimals = decimals;
    e->amount = amount;
}

static void commit_balances(fixture *f) {
    f->tx.pre_token_balances = f->pre;
    f->tx.pre_token_balance_count = f->pre_count;
    f->tx.post_token_balances = f->post;
    f->tx.post_token_balance_count = f->post_count;
}

/* A "Program log: ray_log: <base64>" line over `raw`. */
static void add_raylog(fixture *f, const uint8_t *raw, size_t raw_len) {
    char *buf = f->logbuf[f->log_count];
    int prefix = snprintf(buf, sizeof(f->logbuf[0]), "Program log: ray_log: ");
    size_t b64_len = 0;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_base64_encode(idx_slice_make(raw, raw_len),
                                  buf + prefix,
                                  sizeof(f->logbuf[0]) - (size_t)prefix,
                                  &b64_len, &err),
                IDX_OK);
    f->logs[f->log_count] = idx_slice_make(buf, (size_t)prefix + b64_len);
    f->log_count++;
    f->tx.logs = f->logs;
    f->tx.log_count = f->log_count;
}

static idx_status normalize(fixture *f, const idx_swap_row **rows,
                            size_t *count) {
    idx_error err;
    idx_error_clear(&err);
    idx_status st = idx_swap_normalize(&f->tx, &f->arena, rows, count, &err);
    if (st != IDX_OK) {
        TEST_CHECK(err.file != NULL, "failure recorded no context");
    }
    return st;
}

static const uint8_t ANCHOR_EVENT[8] = {0xe4, 0x45, 0xa5, 0x2e,
                                        0x51, 0xcb, 0x9a, 0x1d};
static const uint8_t TRADE_EVENT[8] = {0xbd, 0xdb, 0x7f, 0xd3,
                                       0x4e, 0xe6, 0x61, 0xee};
static const uint8_t BUY_EVENT[8] = {0x67, 0xf4, 0x52, 0x1f,
                                     0x2c, 0xf5, 0x77, 0x77};
static const uint8_t SWAP_EVENT[8] = {0x40, 0xc6, 0xcd, 0xe8,
                                      0x26, 0x08, 0x71, 0xe2};
static const uint8_t AMM_BUY[8] = {0x66, 0x06, 0x3d, 0x12,
                                   0x01, 0xda, 0xeb, 0xea};
static const uint8_t CLMM_SWAP_V2[8] = {0x2b, 0x04, 0xed, 0x0b,
                                        0x1a, 0xc9, 0x1e, 0x62};

/* Adds a Jupiter SwapEvent leg under top-level 0. */
static void add_jup_leg(fixture *f, uint8_t in_mint, uint64_t in_amt,
                        uint8_t out_mint, uint64_t out_amt) {
    static const uint8_t one[] = {6};
    payload *p = NULL;
    idx_instruction *ix = add_inner(f, 0, PROGRAM_JUPITER, one, 1, &p);
    put_bytes(p, ANCHOR_EVENT, 8);
    put_bytes(p, SWAP_EVENT, 8);
    put_key(p, 0x01); /* amm, dropped */
    put_key(p, in_mint);
    put_u64(p, in_amt);
    put_key(p, out_mint);
    put_u64(p, out_amt);
    seal(ix, p);
}

/* ---------------------------------------------------------------- tests -- */

/*
 * PumpSwap: the event states amounts and the trader's accounts, the
 * instruction states the mints. Normalization pairs them on the pool, so a row
 * comes out with both mints even though the quote account (28) never appears in
 * the token balances — the temporary wrapped-SOL account the real chain uses.
 *
 * Instruction accounts: [0]pool=30 [1]user=31 [3]base_mint=25 [4]quote=WSOL
 * [5]user_base=27 [7]/[8] vaults. The event repeats pool, user and the two
 * user accounts, and carries the amounts.
 */
static void test_pump_amm_pairs_event_with_instruction(void) {
    fixture f;
    fixture_init(&f);

    static const uint8_t buy_accts[] = {30, 31, 32, 25, ACCOUNT_WSOL,
                                        27, 28, 12, 13};
    payload *ib = set_top(&f, 0, PROGRAM_PUMP_AMM, buy_accts, 9);
    put_bytes(ib, AMM_BUY, 8);
    put_u64(ib, 1000);
    put_u64(ib, 2000);
    seal(&f.top[0], ib);

    payload *pe = NULL;
    static const uint8_t evt_accts[] = {30};
    idx_instruction *ev = add_inner(&f, 0, PROGRAM_PUMP_AMM, evt_accts, 1, &pe);
    put_bytes(pe, ANCHOR_EVENT, 8);
    put_bytes(pe, BUY_EVENT, 8);
    put_u64_at(pe, 16 + 8, 7387481318);  /* base_amount_out */
    put_u64_at(pe, 16 + 104, 196428569); /* user_quote_amount_in */
    put_key_at(pe, 16 + 112, 30);        /* pool */
    put_key_at(pe, 16 + 144, 31);        /* user */
    put_key_at(pe, 16 + 176, 27);        /* user_base_account */
    put_key_at(pe, 16 + 208, 28);        /* user_quote_account */
    put_key_at(pe, 16 + 240, 0xee);      /* tail */
    seal(ev, pe);

    /* The base mint (25) has a token balance naming its scale; the quote is
     * WSOL, known without one. */
    add_balance(f.post, &f.post_count, 27, 25, 6, 7387481318);
    commit_balances(&f);

    const idx_swap_row *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(normalize(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].kind, IDX_SWAP_POOL);
    TEST_EQ_INT(rows[0].venue, IDX_VENUE_PUMP_AMM);
    TEST_EQ_INT(rows[0].source, IDX_AMOUNT_EVENT);
    TEST_ASSERT(rows[0].has_pool && is_key(&rows[0].pool, 30));
    /* A buy: quote (WSOL) went in, base came out. */
    TEST_ASSERT(rows[0].has_input_mint);
    TEST_ASSERT(idx_pubkey_equal(&rows[0].input_mint, &IDX_MINT_WSOL));
    TEST_EQ_UINT(rows[0].input_amount, 196428569);
    TEST_ASSERT(rows[0].has_output_mint && is_key(&rows[0].output_mint, 25));
    TEST_EQ_UINT(rows[0].output_amount, 7387481318);
    TEST_ASSERT(rows[0].has_input_decimals && rows[0].input_decimals == 9);
    TEST_ASSERT(rows[0].has_output_decimals && rows[0].output_decimals == 6);

    fixture_free(&f);
}

/* pump.fun's curve event carries everything; the token's own mint is the pool
 * key, and only its decimals need meta. */
static void test_pump_curve_full_from_event(void) {
    fixture f;
    fixture_init(&f);

    static const uint8_t curve_accts[] = {30};
    payload *p = set_top(&f, 0, PROGRAM_PUMP_CURVE, curve_accts, 1);
    put_bytes(p, ANCHOR_EVENT, 8);
    put_bytes(p, TRADE_EVENT, 8);
    put_key(p, 25);            /* mint */
    put_u64(p, 494142551);     /* sol_amount */
    put_u64(p, 4003969271065); /* token_amount */
    put_u8(p, 1);              /* is_buy */
    put_key(p, 31);            /* user */
    seal(&f.top[0], p);

    add_balance(f.post, &f.post_count, 12, 25, 6, 4003969271065);
    commit_balances(&f);

    const idx_swap_row *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(normalize(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].venue, IDX_VENUE_PUMP_CURVE);
    TEST_ASSERT(rows[0].has_pool && is_key(&rows[0].pool, 25)); /* the mint */
    TEST_ASSERT(idx_pubkey_equal(&rows[0].input_mint, &IDX_MINT_WSOL));
    TEST_EQ_UINT(rows[0].input_amount, 494142551);
    TEST_ASSERT(is_key(&rows[0].output_mint, 25));
    TEST_EQ_UINT(rows[0].output_amount, 4003969271065);
    TEST_ASSERT(rows[0].input_decimals == 9 && rows[0].output_decimals == 6);

    fixture_free(&f);
}

/* Raydium v4: the ray_log states both amounts, so it wins over the vault
 * deltas (D9); the mints come from the trader's token accounts. */
static void test_raydium_amm_v4_from_raylog(void) {
    fixture f;
    fixture_init(&f);

    /* 17-account swap: pool[1]=30, vaults[4]=33,[5]=34, trader
     * source/dest/owner at the tail (14,15,16) = 35,36,31. */
    static const uint8_t accts[] = {6,  30, 8,  9,  33, 34, 12, 13, 14,
                                    15, 16, 17, 18, 19, 35, 36, 31};
    payload *p = set_top(&f, 0, PROGRAM_RAYDIUM_AMM, accts, 17);
    put_u8(p, 9);         /* SwapBaseIn */
    put_u64(p, 78491305); /* amount_in, the fixed side */
    put_u64(p, 0);        /* minimum_out */
    seal(&f.top[0], p);

    add_balance(f.post, &f.post_count, 35, 0x99, 9, 0); /* source: WSOL-ish */
    add_balance(f.post, &f.post_count, 36, 0x77, 6, 0); /* dest: token */
    commit_balances(&f);

    uint8_t raw[57];
    memset(raw, 0, sizeof(raw));
    raw[0] = 3;
    uint64_t fields[7] = {78491305, 0, 2, 0, 0, 0, 5794390};
    for (size_t i = 0; i < 7; i++) {
        for (size_t k = 0; k < 8; k++) {
            raw[1 + i * 8 + k] = (uint8_t)((fields[i] >> (8 * k)) & 0xff);
        }
    }
    add_raylog(&f, raw, sizeof(raw));

    const idx_swap_row *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(normalize(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].venue, IDX_VENUE_RAYDIUM_AMM_V4);
    TEST_EQ_INT(rows[0].source, IDX_AMOUNT_RAYLOG);
    TEST_EQ_UINT(rows[0].input_amount, 78491305);
    TEST_EQ_UINT(rows[0].output_amount, 5794390);
    TEST_ASSERT(is_key(&rows[0].input_mint, 0x99));
    TEST_ASSERT(is_key(&rows[0].output_mint, 0x77));

    fixture_free(&f);
}

/* With no ray_log the missing side falls to the vault deltas: the vault holding
 * the output mint fell by the output amount. */
static void test_raydium_amm_v4_delta_fallback(void) {
    fixture f;
    fixture_init(&f);

    static const uint8_t accts[] = {6,  30, 8,  9,  33, 34, 12, 13, 14,
                                    15, 16, 17, 18, 19, 35, 36, 31};
    payload *p = set_top(&f, 0, PROGRAM_RAYDIUM_AMM, accts, 17);
    put_u8(p, 9);
    put_u64(p, 78491305);
    put_u64(p, 0);
    seal(&f.top[0], p);

    add_balance(f.post, &f.post_count, 35, 0x99, 9, 0);
    add_balance(f.post, &f.post_count, 36, 0x77, 6, 0);
    /* Input vault (33, WSOL) rose by the input; output vault (34, token) fell
     * by 5794390. */
    add_balance(f.pre, &f.pre_count, 33, 0x99, 9, 1000000000);
    add_balance(f.post, &f.post_count, 33, 0x99, 9, 1078491305);
    add_balance(f.pre, &f.pre_count, 34, 0x77, 6, 8000000);
    add_balance(f.post, &f.post_count, 34, 0x77, 6, 8000000 - 5794390);
    commit_balances(&f);

    const idx_swap_row *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(normalize(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].source, IDX_AMOUNT_DELTA);
    TEST_EQ_UINT(rows[0].input_amount, 78491305);
    TEST_ASSERT(rows[0].has_output_amount);
    TEST_EQ_UINT(rows[0].output_amount, 5794390);

    fixture_free(&f);
}

/* Raydium CLMM swapV2 names both mints; the missing amount is its output
 * vault's delta, taken by position. */
static void test_raydium_clmm_swap_v2(void) {
    fixture f;
    fixture_init(&f);

    /* [0]payer=31 [2]pool=30 [3]user_in=33 [4]user_out=34 [5]in_vault=35
     * [6]out_vault=36 [11]in_mint=25 [12]out_mint=24. */
    static const uint8_t accts[] = {31, 8, 30, 33, 34, 35, 36,
                                    9,  10, 11, 12, 25, 24};
    payload *p = set_top(&f, 0, PROGRAM_RAYDIUM_CLMM, accts, 13);
    put_bytes(p, CLMM_SWAP_V2, 8);
    put_u64(p, 3061218841); /* amount */
    put_u64(p, 1);          /* threshold */
    for (size_t i = 0; i < 16; i++) {
        put_u8(p, 0); /* sqrt_price_limit */
    }
    put_u8(p, 1); /* is_base_input */
    seal(&f.top[0], p);

    /* Output vault (36) fell by the output amount. */
    add_balance(f.pre, &f.pre_count, 36, 24, 6, 9000000000);
    add_balance(f.post, &f.post_count, 36, 24, 6, 9000000000 - 42424242);
    commit_balances(&f);

    const idx_swap_row *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(normalize(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].venue, IDX_VENUE_RAYDIUM_CLMM);
    TEST_ASSERT(is_key(&rows[0].input_mint, 25));
    TEST_ASSERT(is_key(&rows[0].output_mint, 24));
    TEST_EQ_UINT(rows[0].input_amount, 3061218841); /* fixed by the payload */
    TEST_EQ_UINT(rows[0].output_amount, 42424242);  /* from the vault delta */

    fixture_free(&f);
}

/* A three-leg route A->B->C->D nets to A and D; the legs belong to their own
 * venues, not to this row (D8). */
static void test_jupiter_route_endpoints(void) {
    fixture f;
    fixture_init(&f);

    static const uint8_t r[] = {6, 7, 8};
    payload *route = set_top(&f, 0, PROGRAM_JUPITER, r, 3);
    put_u64(route, 1);
    seal(&f.top[0], route);

    add_jup_leg(&f, 0x41, 634524165458, 0x42, 2811151606); /* A -> B */
    add_jup_leg(&f, 0x42, 2811151606, 0x43, 900000000);    /* B -> C */
    add_jup_leg(&f, 0x43, 900000000, 0x44, 207536010);     /* C -> D */

    add_balance(f.post, &f.post_count, 20, 0x41, 6, 0);
    add_balance(f.post, &f.post_count, 21, 0x44, 9, 0);
    commit_balances(&f);

    const idx_swap_row *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(normalize(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].kind, IDX_SWAP_AGGREGATED);
    TEST_EQ_INT(rows[0].venue, IDX_VENUE_JUPITER);
    TEST_ASSERT(is_key(&rows[0].input_mint, 0x41));
    TEST_EQ_UINT(rows[0].input_amount, 634524165458);
    TEST_ASSERT(is_key(&rows[0].output_mint, 0x44));
    TEST_EQ_UINT(rows[0].output_amount, 207536010);
    TEST_ASSERT(rows[0].input_decimals == 6 && rows[0].output_decimals == 9);
    TEST_ASSERT(rows[0].has_user); /* the fee payer */

    fixture_free(&f);
}

/* A split route — A into B over two pools, then B into C — still nets to A and
 * C because the intermediate cancels. */
static void test_jupiter_split_route(void) {
    fixture f;
    fixture_init(&f);

    static const uint8_t r[] = {6};
    payload *route = set_top(&f, 0, PROGRAM_JUPITER, r, 1);
    put_u64(route, 1);
    seal(&f.top[0], route);

    add_jup_leg(&f, 0x41, 600, 0x42, 300);
    add_jup_leg(&f, 0x41, 400, 0x42, 200);
    add_jup_leg(&f, 0x42, 500, 0x43, 999);
    commit_balances(&f);

    const idx_swap_row *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(normalize(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].kind, IDX_SWAP_AGGREGATED);
    TEST_ASSERT(is_key(&rows[0].input_mint, 0x41));
    TEST_EQ_UINT(rows[0].input_amount, 1000); /* 600 + 400 */
    TEST_ASSERT(is_key(&rows[0].output_mint, 0x43));
    TEST_EQ_UINT(rows[0].output_amount, 999);

    fixture_free(&f);
}

/* A cyclic route back to its start nets to nothing on the opposite end: a
 * self-trade, no completed-trade row (D8). */
static void test_jupiter_cyclic_route(void) {
    fixture f;
    fixture_init(&f);

    static const uint8_t r[] = {6};
    payload *route = set_top(&f, 0, PROGRAM_JUPITER, r, 1);
    put_u64(route, 1);
    seal(&f.top[0], route);

    add_jup_leg(&f, 0x41, 1000, 0x42, 500);
    add_jup_leg(&f, 0x42, 500, 0x41, 1010); /* back to A, a profit */
    commit_balances(&f);

    const idx_swap_row *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(normalize(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);

    fixture_free(&f);
}

/* Failed and metadata-less transactions yield nothing, as everywhere else. */
static void test_guards(void) {
    fixture f;
    fixture_init(&f);
    static const uint8_t curve_accts[] = {30};
    payload *p = set_top(&f, 0, PROGRAM_PUMP_CURVE, curve_accts, 1);
    put_bytes(p, ANCHOR_EVENT, 8);
    put_bytes(p, TRADE_EVENT, 8);
    put_key(p, 25);
    put_u64(p, 1);
    put_u64(p, 2);
    put_u8(p, 1);
    put_key(p, 31);
    seal(&f.top[0], p);
    commit_balances(&f);

    const idx_swap_row *rows = NULL;
    size_t count = 0;
    f.tx.success = false;
    TEST_EQ_INT(normalize(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);

    f.tx.success = true;
    f.tx.has_meta = false;
    TEST_EQ_INT(normalize(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);

    TEST_EQ_INT(idx_swap_normalize(NULL, &f.arena, &rows, &count, NULL),
                IDX_ERR_INVALID_ARG);

    fixture_free(&f);
}

static void test_source_names(void) {
    TEST_EQ_STR(idx_amount_source_name(IDX_AMOUNT_EVENT), "event");
    TEST_EQ_STR(idx_amount_source_name(IDX_AMOUNT_RAYLOG), "raylog");
    TEST_EQ_STR(idx_amount_source_name(IDX_AMOUNT_DELTA), "delta");
    TEST_EQ_STR(idx_amount_source_name(IDX_AMOUNT_NONE), "none");
}

TEST_MAIN({
    TEST_RUN(test_pump_amm_pairs_event_with_instruction);
    TEST_RUN(test_pump_curve_full_from_event);
    TEST_RUN(test_raydium_amm_v4_from_raylog);
    TEST_RUN(test_raydium_amm_v4_delta_fallback);
    TEST_RUN(test_raydium_clmm_swap_v2);
    TEST_RUN(test_jupiter_route_endpoints);
    TEST_RUN(test_jupiter_split_route);
    TEST_RUN(test_jupiter_cyclic_route);
    TEST_RUN(test_guards);
    TEST_RUN(test_source_names);
})
