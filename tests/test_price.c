/*
 * Swap pricing. The set is built from strings — names and base58 mints, in
 * priority order — and the pricer is fed swap rows directly, since that is what
 * it takes: mints, amounts and decimals, whatever their source. The cases cover
 * the two questions it answers — which side is the quote, and what the number
 * is — plus the honest not-a-number outcomes D5 asks for.
 */
#include "price.h"

#include <math.h>
#include <string.h>

#include "test.h"
#include "venue.h"

static idx_pubkey key_fill(uint8_t fill) {
    idx_pubkey k;
    memset(k.bytes, fill, IDX_PUBKEY_LEN);
    return k;
}

/* The mint of the first entry of `kind` in `set`, for building rows against the
 * same addresses the set holds. */
static idx_pubkey mint_of_kind(const idx_quote_set *set, idx_quote kind) {
    for (size_t i = 0; i < set->count; i++) {
        if (set->mints[i].kind == kind) {
            return set->mints[i].mint;
        }
    }
    return key_fill(0);
}

static bool close_to(double a, double b) { return fabs(a - b) < 1e-9; }

/* ---------------------------------------------------------------- sets -- */

static void test_defaults(void) {
    idx_quote_set set;
    idx_quote_set_defaults(&set);
    TEST_EQ_UINT(set.count, 4);
    /* The documented priority order: dollars ahead of SOL (D10). */
    TEST_EQ_INT(set.mints[0].kind, IDX_QUOTE_USDC);
    TEST_EQ_INT(set.mints[1].kind, IDX_QUOTE_USDT);
    TEST_EQ_INT(set.mints[2].kind, IDX_QUOTE_USD1);
    TEST_EQ_INT(set.mints[3].kind, IDX_QUOTE_SOL);
    /* The "sol" name resolves to the very mint venue.c exposes. */
    TEST_ASSERT(idx_pubkey_equal(&set.mints[3].mint, &IDX_MINT_WSOL));
}

static void test_parse_names_and_order(void) {
    idx_quote_set set;
    idx_error err;
    idx_error_clear(&err);
    /* wsol is an alias for sol; the order given is the priority. */
    TEST_EQ_INT(idx_quote_set_parse(&set, "wsol, usdc", &err), IDX_OK);
    TEST_EQ_UINT(set.count, 2);
    TEST_EQ_INT(set.mints[0].kind, IDX_QUOTE_SOL);
    TEST_EQ_INT(set.mints[1].kind, IDX_QUOTE_USDC);
}

static void test_parse_base58_other(void) {
    idx_quote_set set;
    idx_error err;
    idx_error_clear(&err);
    /* A base58 mint with no well-known name becomes an OTHER entry. */
    TEST_EQ_INT(idx_quote_set_parse(
                    &set, "So11111111111111111111111111111111111111112", &err),
                IDX_OK);
    TEST_EQ_UINT(set.count, 1);
    TEST_EQ_INT(set.mints[0].kind, IDX_QUOTE_OTHER);
    TEST_ASSERT(idx_pubkey_equal(&set.mints[0].mint, &IDX_MINT_WSOL));
}

static void test_parse_dedup_keeps_first(void) {
    idx_quote_set set;
    idx_error err;
    idx_error_clear(&err);
    /* sol and its base58 are the same mint; the first mention keeps the rank. */
    TEST_EQ_INT(idx_quote_set_parse(
                    &set,
                    "sol,usdc,So11111111111111111111111111111111111111112", &err),
                IDX_OK);
    TEST_EQ_UINT(set.count, 2);
    TEST_EQ_INT(set.mints[0].kind, IDX_QUOTE_SOL);
    TEST_EQ_INT(set.mints[1].kind, IDX_QUOTE_USDC);
}

static void test_parse_empty(void) {
    idx_quote_set set;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_quote_set_parse(&set, "", &err), IDX_OK);
    TEST_EQ_UINT(set.count, 0);
    /* A trailing comma and spaces are empty fields, not entries. */
    TEST_EQ_INT(idx_quote_set_parse(&set, " usdc , ", &err), IDX_OK);
    TEST_EQ_UINT(set.count, 1);
}

static void test_parse_errors(void) {
    idx_quote_set set;
    idx_error err;
    idx_error_clear(&err);
    /* Not a name and not a base58 key. */
    TEST_EQ_INT(idx_quote_set_parse(&set, "usdc,notacoin", &err), IDX_ERR_PARSE);
    /* Longer than any name or base58 pubkey. */
    char big[64];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    TEST_EQ_INT(idx_quote_set_parse(&set, big, &err), IDX_ERR_RANGE);
    TEST_EQ_INT(idx_quote_set_parse(NULL, "usdc", &err), IDX_ERR_INVALID_ARG);
}

static void test_names(void) {
    TEST_EQ_STR(idx_quote_name(IDX_QUOTE_NONE), "none");
    TEST_EQ_STR(idx_quote_name(IDX_QUOTE_SOL), "sol");
    TEST_EQ_STR(idx_quote_name(IDX_QUOTE_USDC), "usdc");
    TEST_EQ_STR(idx_quote_name(IDX_QUOTE_USDT), "usdt");
    TEST_EQ_STR(idx_quote_name(IDX_QUOTE_USD1), "usd1");
    TEST_EQ_STR(idx_quote_name(IDX_QUOTE_OTHER), "other");
}

/* -------------------------------------------------------------- pricing -- */

/* A base row with both mints, amounts and decimals set; tests override sides. */
static idx_swap_row make_row(idx_pubkey in_mint, uint64_t in_amt, uint8_t in_dec,
                             idx_pubkey out_mint, uint64_t out_amt,
                             uint8_t out_dec) {
    idx_swap_row row;
    memset(&row, 0, sizeof(row));
    row.kind = IDX_SWAP_POOL;
    row.input_mint = in_mint;
    row.has_input_mint = true;
    row.input_amount = in_amt;
    row.has_input_amount = true;
    row.input_decimals = in_dec;
    row.has_input_decimals = true;
    row.output_mint = out_mint;
    row.has_output_mint = true;
    row.output_amount = out_amt;
    row.has_output_amount = true;
    row.output_decimals = out_dec;
    row.has_output_decimals = true;
    return row;
}

/* A buy of a token with SOL: SOL leaves (input), the token arrives (output). The
 * quote is the sole quote side, SOL, and the price is SOL per token. */
static void test_price_token_against_sol(void) {
    idx_quote_set set;
    idx_quote_set_defaults(&set);
    idx_pubkey token = key_fill(0x25);

    /* 0.5 SOL in (9 dec), 1.0 token out (6 dec) -> 0.5 SOL per token. */
    idx_swap_row row =
        make_row(IDX_MINT_WSOL, 500000000, 9, token, 1000000, 6);

    idx_price price;
    TEST_ASSERT(idx_price_of_swap(&set, &row, &price));
    TEST_ASSERT(price.priced && price.has_price);
    TEST_EQ_INT(price.quote, IDX_QUOTE_SOL);
    TEST_ASSERT(idx_pubkey_equal(&price.quote_mint, &IDX_MINT_WSOL));
    TEST_ASSERT(idx_pubkey_equal(&price.base_mint, &token));
    TEST_ASSERT(close_to(price.price, 0.5));
}

/* When both sides are quotes — a SOL/USDC pool — the dollar quote wins by
 * priority, so the price is USDC per SOL rather than the reverse (D10). */
static void test_price_prefers_higher_priority_quote(void) {
    idx_quote_set set;
    idx_quote_set_defaults(&set);
    idx_pubkey usdc = mint_of_kind(&set, IDX_QUOTE_USDC);

    /* 1 SOL in (9 dec), 200 USDC out (6 dec) -> 200 USDC per SOL. */
    idx_swap_row row = make_row(IDX_MINT_WSOL, 1000000000, 9, usdc, 200000000, 6);

    idx_price price;
    TEST_ASSERT(idx_price_of_swap(&set, &row, &price));
    TEST_ASSERT(price.has_price);
    TEST_EQ_INT(price.quote, IDX_QUOTE_USDC);
    TEST_ASSERT(idx_pubkey_equal(&price.base_mint, &IDX_MINT_WSOL));
    TEST_ASSERT(close_to(price.price, 200.0));
}

/* A trade between two non-quote tokens is a swap but not a price (D5). */
static void test_price_no_quote_side(void) {
    idx_quote_set set;
    idx_quote_set_defaults(&set);
    idx_swap_row row =
        make_row(key_fill(0x25), 1000, 6, key_fill(0x26), 2000, 6);
    idx_price price;
    TEST_ASSERT(!idx_price_of_swap(&set, &row, &price));
    TEST_ASSERT(!price.priced);
    TEST_ASSERT(!price.has_price);
}

/* A quote side whose base counterpart the block never scaled is priced but
 * numberless: the quote is known, the number is not. */
static void test_price_priced_but_numberless(void) {
    idx_quote_set set;
    idx_quote_set_defaults(&set);
    idx_pubkey token = key_fill(0x25);
    idx_swap_row row =
        make_row(IDX_MINT_WSOL, 500000000, 9, token, 1000000, 6);
    row.has_output_decimals = false; /* the base scale is unknown */

    idx_price price;
    TEST_ASSERT(idx_price_of_swap(&set, &row, &price));
    TEST_ASSERT(price.priced);
    TEST_ASSERT(!price.has_price);
    TEST_EQ_INT(price.quote, IDX_QUOTE_SOL);
}

/* A zero base amount has no price rather than a division by zero. */
static void test_price_zero_base(void) {
    idx_quote_set set;
    idx_quote_set_defaults(&set);
    idx_pubkey token = key_fill(0x25);
    idx_swap_row row = make_row(IDX_MINT_WSOL, 500000000, 9, token, 0, 6);
    idx_price price;
    TEST_ASSERT(idx_price_of_swap(&set, &row, &price));
    TEST_ASSERT(price.priced && !price.has_price);
}

/* An empty set prices nothing, and NULL arguments are refused without a crash. */
static void test_price_guards(void) {
    idx_quote_set empty;
    memset(&empty, 0, sizeof(empty));
    idx_pubkey token = key_fill(0x25);
    idx_swap_row row =
        make_row(IDX_MINT_WSOL, 500000000, 9, token, 1000000, 6);

    idx_price price;
    TEST_ASSERT(!idx_price_of_swap(&empty, &row, &price));
    TEST_ASSERT(!price.priced);

    TEST_ASSERT(!idx_price_of_swap(NULL, &row, &price));
    TEST_ASSERT(!idx_price_of_swap(&empty, NULL, &price));
    TEST_ASSERT(!idx_price_of_swap(&empty, &row, NULL));
}

/* An aggregated route row is priced on its netted endpoints like any other. */
static void test_price_route_row(void) {
    idx_quote_set set;
    idx_quote_set_defaults(&set);
    idx_pubkey token = key_fill(0x44);
    idx_swap_row row = make_row(token, 1000000, 6, IDX_MINT_WSOL, 500000000, 9);
    row.kind = IDX_SWAP_AGGREGATED;

    idx_price price;
    TEST_ASSERT(idx_price_of_swap(&set, &row, &price));
    TEST_ASSERT(price.has_price);
    TEST_EQ_INT(price.quote, IDX_QUOTE_SOL);
    TEST_ASSERT(close_to(price.price, 0.5)); /* 0.5 SOL per token */
}

TEST_MAIN({
    TEST_RUN(test_defaults);
    TEST_RUN(test_parse_names_and_order);
    TEST_RUN(test_parse_base58_other);
    TEST_RUN(test_parse_dedup_keeps_first);
    TEST_RUN(test_parse_empty);
    TEST_RUN(test_parse_errors);
    TEST_RUN(test_names);
    TEST_RUN(test_price_token_against_sol);
    TEST_RUN(test_price_prefers_higher_priority_quote);
    TEST_RUN(test_price_no_quote_side);
    TEST_RUN(test_price_priced_but_numberless);
    TEST_RUN(test_price_zero_base);
    TEST_RUN(test_price_guards);
    TEST_RUN(test_price_route_row);
})
