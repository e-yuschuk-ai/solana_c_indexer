#include "price.h"

#include <string.h>
#include <strings.h>

#include "venue.h"

/*
 * Well-known quote mints, by base58. Decoded once when a set is built rather
 * than kept as raw bytes like the program ids in venue.c: a quote set is
 * configuration, assembled at startup from strings a deployment may override,
 * so the same string-to-key path serves the defaults and the overrides, and a
 * handful of decodes at startup costs nothing. test_price cross-checks each
 * against the address here.
 */
static const struct {
    const char *name;
    idx_quote kind;
    const char *mint;
} k_named_quotes[] = {
    {"sol", IDX_QUOTE_SOL, "So11111111111111111111111111111111111111112"},
    {"wsol", IDX_QUOTE_SOL, "So11111111111111111111111111111111111111112"},
    {"usdc", IDX_QUOTE_USDC, "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"},
    {"usdt", IDX_QUOTE_USDT, "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB"},
    {"usd1", IDX_QUOTE_USD1, "USD1ttGY1N17NEEHLmELoaybftRBUSErhqYiQzvEmuB"},
};

const char *idx_quote_name(idx_quote quote) {
    switch (quote) {
    case IDX_QUOTE_NONE:
        return "none";
    case IDX_QUOTE_SOL:
        return "sol";
    case IDX_QUOTE_USDC:
        return "usdc";
    case IDX_QUOTE_USDT:
        return "usdt";
    case IDX_QUOTE_USD1:
        return "usd1";
    case IDX_QUOTE_OTHER:
        return "other";
    }
    return "unknown";
}

/* --------------------------------------------------------- set building -- */

/* Adds one mint at the next-lowest priority, unless it is already present — the
 * first mention wins its rank, so a repeat is dropped rather than demoting the
 * original. */
static idx_status add_quote(idx_quote_set *set, const idx_pubkey *mint,
                           idx_quote kind, idx_error *err) {
    for (size_t i = 0; i < set->count; i++) {
        if (idx_pubkey_equal(&set->mints[i].mint, mint)) {
            return IDX_OK;
        }
    }
    if (set->count >= IDX_QUOTE_SET_MAX) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "too many quote mints, maximum is %d", IDX_QUOTE_SET_MAX);
    }
    set->mints[set->count].mint = *mint;
    set->mints[set->count].kind = kind;
    set->count++;
    return IDX_OK;
}

/* Resolves one entry — a well-known name or a base58 mint — and adds it. */
static idx_status add_entry(idx_quote_set *set, const char *token,
                           idx_error *err) {
    for (size_t i = 0; i < sizeof(k_named_quotes) / sizeof(k_named_quotes[0]);
         i++) {
        if (strcasecmp(token, k_named_quotes[i].name) == 0) {
            idx_pubkey mint;
            /* A named quote's address is a compile-time constant, so a decode
             * failure here is a bug in this table, not bad input. */
            IDX_TRY(idx_pubkey_from_base58(k_named_quotes[i].mint,
                                           strlen(k_named_quotes[i].mint), &mint,
                                           err));
            return add_quote(set, &mint, k_named_quotes[i].kind, err);
        }
    }

    idx_pubkey mint;
    idx_status status =
        idx_pubkey_from_base58(token, strlen(token), &mint, err);
    if (status != IDX_OK) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "quote mint '%s' is neither a known name nor a base58 "
                        "address",
                        token);
    }
    return add_quote(set, &mint, IDX_QUOTE_OTHER, err);
}

idx_status idx_quote_set_parse(idx_quote_set *set, const char *spec,
                               idx_error *err) {
    if (set == NULL || spec == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "set and spec must not be NULL");
    }
    memset(set, 0, sizeof(*set));

    const char *p = spec;
    while (*p != '\0') {
        /* Skip separators and leading spaces. */
        while (*p == ',' || *p == ' ' || *p == '\t') {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        /* Trim trailing spaces of this token. */
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        if (end == start) {
            continue; /* an empty field, e.g. a trailing comma */
        }

        size_t len = (size_t)(end - start);
        char token[IDX_PUBKEY_STR_MAX];
        if (len >= sizeof(token)) {
            return IDX_FAIL(err, IDX_ERR_RANGE,
                            "quote mint entry is %zu bytes, too long for a name "
                            "or a base58 key",
                            len);
        }
        memcpy(token, start, len);
        token[len] = '\0';

        IDX_TRY(add_entry(set, token, err));
    }
    return IDX_OK;
}

void idx_quote_set_defaults(idx_quote_set *set) {
    if (set == NULL) {
        return;
    }
    /* The default entries are all well-known names, so this cannot fail; if it
     * ever did, an empty set is the safe outcome — pricing nothing beats
     * pricing against a half-built set. test_price asserts the four are here. */
    if (idx_quote_set_parse(set, "usdc,usdt,usd1,sol", NULL) != IDX_OK) {
        memset(set, 0, sizeof(*set));
    }
}

/* ------------------------------------------------------------- pricing -- */

/* 10^n as a double, for n within a mint's decimals — a small exponent, so a
 * repeated multiply is exact up to the range doubles represent exactly and
 * avoids a libm dependency. */
static double pow10_double(uint8_t n) {
    double result = 1.0;
    for (uint8_t i = 0; i < n; i++) {
        result *= 10.0;
    }
    return result;
}

/* The priority rank of `mint` in `set` (0 is highest), or -1 when absent. */
static int quote_rank(const idx_quote_set *set, const idx_pubkey *mint,
                      idx_quote *kind) {
    for (size_t i = 0; i < set->count; i++) {
        if (idx_pubkey_equal(&set->mints[i].mint, mint)) {
            if (kind != NULL) {
                *kind = set->mints[i].kind;
            }
            return (int)i;
        }
    }
    return -1;
}

bool idx_price_of_swap(const idx_quote_set *set, const idx_swap_row *row,
                       idx_price *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (set == NULL || row == NULL) {
        return false;
    }

    idx_quote input_kind = IDX_QUOTE_NONE;
    idx_quote output_kind = IDX_QUOTE_NONE;
    int input_rank =
        row->has_input_mint ? quote_rank(set, &row->input_mint, &input_kind) : -1;
    int output_rank = row->has_output_mint
                          ? quote_rank(set, &row->output_mint, &output_kind)
                          : -1;

    if (input_rank < 0 && output_rank < 0) {
        return false; /* neither side is a quote: a swap, but no price (D5) */
    }

    /*
     * Which side is the quote. When only one side qualifies it is that one;
     * when both do, the higher priority (the smaller rank) wins, so the base is
     * the lower-priority side (D10). A lower rank number is a higher priority.
     */
    bool quote_is_input;
    if (input_rank < 0) {
        quote_is_input = false;
    } else if (output_rank < 0) {
        quote_is_input = true;
    } else {
        quote_is_input = input_rank <= output_rank;
    }

    out->priced = true;
    if (quote_is_input) {
        out->quote = input_kind;
        out->quote_mint = row->input_mint;
        out->quote_amount = row->input_amount;
        out->quote_decimals = row->input_decimals;
        out->base_mint = row->output_mint;
        out->base_amount = row->output_amount;
        out->base_decimals = row->output_decimals;
    } else {
        out->quote = output_kind;
        out->quote_mint = row->output_mint;
        out->quote_amount = row->output_amount;
        out->quote_decimals = row->output_decimals;
        out->base_mint = row->input_mint;
        out->base_amount = row->input_amount;
        out->base_decimals = row->input_decimals;
    }

    /* The number needs both mints, both amounts, both scales, and something to
     * divide by. The quote side is a quote mint, so it is known; the base side
     * may not be, which is what leaves a row priced but numberless. */
    bool have_base_mint =
        quote_is_input ? row->has_output_mint : row->has_input_mint;
    bool have_amounts = row->has_input_amount && row->has_output_amount;
    bool have_decimals =
        row->has_input_decimals && row->has_output_decimals;
    if (have_base_mint && have_amounts && have_decimals &&
        out->base_amount != 0) {
        double quote = (double)out->quote_amount / pow10_double(out->quote_decimals);
        double base = (double)out->base_amount / pow10_double(out->base_decimals);
        out->price = quote / base;
        out->has_price = true;
    }
    return true;
}
