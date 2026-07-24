#include "swap.h"

#include <string.h>

#include "base64.h"
#include "bytes.h"
#include "venue_pump.h"
#include "venue_raydium.h"

/* WSOL and native SOL both scale by 9, which the curve's SOL side needs even
 * when no wrapped-SOL token balance is present to read it from. */
#define IDX_WSOL_DECIMALS 9

/* A route rarely touches more than a handful of mints; past this the netting
 * gives up rather than grow, and the transaction produces no aggregated row. */
#define MAX_ROUTE_MINTS 24

const char *idx_amount_source_name(idx_amount_source source) {
    switch (source) {
    case IDX_AMOUNT_NONE:
        return "none";
    case IDX_AMOUNT_EVENT:
        return "event";
    case IDX_AMOUNT_RAYLOG:
        return "raylog";
    case IDX_AMOUNT_DELTA:
        return "delta";
    }
    return "unknown";
}

/* One mint's running total across a route's legs, for the netting D8 does. */
typedef struct {
    idx_pubkey mint;
    uint64_t in_sum;
    uint64_t out_sum;
} route_mint;

typedef struct {
    const idx_transaction *tx;
    idx_swap_row *rows;
    size_t count;
    size_t capacity;

    uint16_t instruction_index;
    uint16_t inner_index;
    bool inner;

    /* ray_log swap lines, in the order they were logged, handed to Raydium v4
     * invocations in the order those are walked. */
    const idx_raydium_swap_log *raylogs;
    size_t raylog_count;
    size_t raylog_next;

    /* Jupiter route accumulation. */
    route_mint net[MAX_ROUTE_MINTS];
    size_t net_count;
    bool net_overflow;
    bool saw_route;
    uint16_t route_top_index;
} builder;

/* ------------------------------------------------------ meta resolution -- */

/* The token balance meta carries for `account`, post side first because it is
 * the transaction's own final view, or NULL. */
static const idx_token_balance *token_balance_of(const idx_transaction *tx,
                                                 const idx_pubkey *account) {
    const idx_token_balance *lists[2] = {tx->post_token_balances,
                                         tx->pre_token_balances};
    size_t counts[2] = {tx->post_token_balance_count,
                        tx->pre_token_balance_count};
    for (size_t l = 0; l < 2; l++) {
        for (size_t i = 0; i < counts[l]; i++) {
            const idx_token_balance *e = &lists[l][i];
            if (idx_pubkey_equal(&tx->accounts[e->account_index].pubkey,
                                 account)) {
                return e;
            }
        }
    }
    return NULL;
}

/* Fills a side's mint and decimals from the token account that holds it. */
static void resolve_side_from_account(const idx_transaction *tx,
                                      const idx_pubkey *account,
                                      idx_pubkey *mint, bool *has_mint,
                                      uint8_t *decimals, bool *has_decimals) {
    const idx_token_balance *e = token_balance_of(tx, account);
    if (e == NULL) {
        return;
    }
    *mint = e->mint;
    *has_mint = true;
    *decimals = e->decimals;
    *has_decimals = true;
}

/* The decimals of a mint, from any token balance that names it. WSOL is known
 * without one, which is what the curve's native-SOL side relies on. */
static bool decimals_of_mint(const idx_transaction *tx, const idx_pubkey *mint,
                             uint8_t *out) {
    if (idx_pubkey_equal(mint, &IDX_MINT_WSOL)) {
        *out = IDX_WSOL_DECIMALS;
        return true;
    }
    const idx_token_balance *lists[2] = {tx->post_token_balances,
                                         tx->pre_token_balances};
    size_t counts[2] = {tx->post_token_balance_count,
                        tx->pre_token_balance_count};
    for (size_t l = 0; l < 2; l++) {
        for (size_t i = 0; i < counts[l]; i++) {
            if (idx_pubkey_equal(&lists[l][i].mint, mint)) {
                *out = lists[l][i].decimals;
                return true;
            }
        }
    }
    return false;
}

/* The signed change of a token account's balance across the transaction, as a
 * magnitude and a direction. A vault that received the input increased; one
 * that paid the output decreased. */
static bool vault_delta(const idx_transaction *tx, const idx_pubkey *vault,
                        uint64_t *magnitude, bool *increased) {
    uint64_t pre = 0;
    uint64_t post = 0;
    bool found = false;
    for (size_t i = 0; i < tx->pre_token_balance_count; i++) {
        const idx_token_balance *e = &tx->pre_token_balances[i];
        if (idx_pubkey_equal(&tx->accounts[e->account_index].pubkey, vault)) {
            pre = e->amount;
            found = true;
            break;
        }
    }
    for (size_t i = 0; i < tx->post_token_balance_count; i++) {
        const idx_token_balance *e = &tx->post_token_balances[i];
        if (idx_pubkey_equal(&tx->accounts[e->account_index].pubkey, vault)) {
            post = e->amount;
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    *increased = post >= pre;
    *magnitude = *increased ? post - pre : pre - post;
    return true;
}

/* The magnitude of the delta of whichever of the two vaults holds `mint`. */
static bool amount_from_vault_of_mint(const idx_transaction *tx,
                                      const idx_pubkey *vault_a,
                                      const idx_pubkey *vault_b,
                                      const idx_pubkey *mint, uint64_t *out) {
    const idx_pubkey *vaults[2] = {vault_a, vault_b};
    for (size_t i = 0; i < 2; i++) {
        const idx_token_balance *e = token_balance_of(tx, vaults[i]);
        if (e != NULL && idx_pubkey_equal(&e->mint, mint)) {
            uint64_t magnitude = 0;
            bool increased = false;
            if (vault_delta(tx, vaults[i], &magnitude, &increased)) {
                *out = magnitude;
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------- ray_log -- */

/* The offset of `needle` in `hay`, or hay.len when absent. */
static size_t slice_find(idx_slice hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay.len < nlen) {
        return hay.len;
    }
    for (size_t i = 0; i + nlen <= hay.len; i++) {
        if (memcmp(hay.data + i, needle, nlen) == 0) {
            return i;
        }
    }
    return hay.len;
}

/*
 * Pulls every swap ray_log out of the logs, decoded and in order. A Raydium v4
 * swap logs one, so the k-th line pairs with the k-th v4 invocation; the caller
 * cross-checks the pairing against the amount the instruction itself fixed.
 */
static idx_status collect_raylogs(const idx_transaction *tx, idx_arena *arena,
                                  const idx_raydium_swap_log **out,
                                  size_t *out_count, idx_error *err) {
    *out = NULL;
    *out_count = 0;
    if (tx->log_count == 0) {
        return IDX_OK;
    }

    /* At most one per log line. */
    void *raw = NULL;
    IDX_TRY(idx_arena_calloc(arena, tx->log_count,
                             sizeof(idx_raydium_swap_log), &raw, err));
    idx_raydium_swap_log *list = raw;
    size_t count = 0;

    static const char MARKER[] = "ray_log: ";
    for (size_t i = 0; i < tx->log_count; i++) {
        idx_slice line = tx->logs[i];
        size_t at = slice_find(line, MARKER);
        if (at == line.len) {
            continue;
        }
        size_t start = at + (sizeof(MARKER) - 1);
        idx_slice b64 = idx_slice_sub(line, start, line.len - start);

        /* The decoded line is 57 bytes for a swap; a generous cap covers the
         * other log types, which are skipped anyway. */
        uint8_t decoded[96];
        size_t decoded_len = 0;
        if (idx_base64_decode((const char *)b64.data, b64.len, decoded,
                              sizeof(decoded), &decoded_len, NULL) != IDX_OK) {
            continue; /* not a ray_log we can read, or one that overran */
        }
        idx_raydium_swap_log parsed;
        if (idx_raydium_swap_log_parse(idx_slice_make(decoded, decoded_len),
                                       &parsed, NULL) == IDX_OK) {
            list[count++] = parsed;
        }
    }
    *out = list;
    *out_count = count;
    return IDX_OK;
}

/* ---------------------------------------------------------- pool rows -- */

/* The row to fill, stamped with where the walk is, or NULL when full (which
 * cannot happen: capacity is one per instruction plus the route row). */
static idx_swap_row *builder_row(builder *b, idx_venue venue,
                                 idx_amount_source source) {
    if (b->count >= b->capacity) {
        return NULL;
    }
    idx_swap_row *row = &b->rows[b->count];
    memset(row, 0, sizeof(*row));
    row->kind = IDX_SWAP_POOL;
    row->venue = venue;
    row->source = source;
    row->instruction_index = b->instruction_index;
    row->inner_index = b->inner_index;
    row->inner = b->inner;
    return row;
}

/* pump.fun's curve trades native SOL against the token; the event states both
 * mints and amounts, and the token's own mint is the pool identity, there
 * being one curve per mint. */
static void normalize_pump_curve(builder *b, const idx_swap *sw,
                                 idx_swap_row *row) {
    row->user = sw->user;
    row->has_user = sw->has_user;
    row->input_mint = sw->input_mint;
    row->has_input_mint = sw->has_input_mint;
    row->input_amount = sw->input_amount;
    row->has_input_amount = sw->has_input_amount;
    row->output_mint = sw->output_mint;
    row->has_output_mint = sw->has_output_mint;
    row->output_amount = sw->output_amount;
    row->has_output_amount = sw->has_output_amount;

    /* The token side is whichever end is not WSOL, and it is the pool key. */
    const idx_pubkey *token = idx_pubkey_equal(&sw->input_mint, &IDX_MINT_WSOL)
                                  ? &sw->output_mint
                                  : &sw->input_mint;
    row->pool = *token;
    row->has_pool = true;

    row->has_input_decimals =
        decimals_of_mint(b->tx, &sw->input_mint, &row->input_decimals);
    row->has_output_decimals =
        decimals_of_mint(b->tx, &sw->output_mint, &row->output_decimals);
}

/* The structural accounts of the PumpSwap swap instruction for `pool`, found
 * by walking the transaction. The mints and vaults live on the instruction,
 * not on the event that carried the amounts, so the two are paired on the pool
 * — which is unambiguous even for the same pool traded twice, since the mints
 * are identical either way. */
static bool find_pump_amm_accounts(const idx_transaction *tx,
                                   const idx_pubkey *pool,
                                   idx_pump_amm_accounts *out) {
    for (size_t i = 0; i < tx->instruction_count; i++) {
        if (idx_venue_pump_amm_accounts(tx, &tx->instructions[i], out) &&
            idx_pubkey_equal(&out->pool, pool)) {
            return true;
        }
    }
    for (size_t g = 0; g < tx->inner_instruction_count; g++) {
        const idx_inner_instructions *group = &tx->inner_instructions[g];
        for (size_t j = 0; j < group->instruction_count; j++) {
            if (idx_venue_pump_amm_accounts(tx, &group->instructions[j], out) &&
                idx_pubkey_equal(&out->pool, pool)) {
                return true;
            }
        }
    }
    return false;
}

/* Sets a resolved side's mint and its decimals. */
static void set_side_mint(const idx_transaction *tx, const idx_pubkey *mint,
                          idx_pubkey *out_mint, bool *has_mint,
                          uint8_t *decimals, bool *has_decimals) {
    *out_mint = *mint;
    *has_mint = true;
    *has_decimals = decimals_of_mint(tx, mint, decimals);
}

/*
 * PumpSwap's event names the pool, the trader, the two token accounts and both
 * amounts, but not the mints. The mints come from the swap instruction, paired
 * on the pool; which one is input is decided by whether the input side is the
 * trader's base account, which the event does name.
 */
static void normalize_pump_amm(builder *b, const idx_swap *sw,
                               idx_swap_row *row) {
    row->pool = sw->pool;
    row->has_pool = sw->has_pool;
    row->user = sw->user;
    row->has_user = sw->has_user;
    row->input_amount = sw->input_amount;
    row->has_input_amount = sw->has_input_amount;
    row->output_amount = sw->output_amount;
    row->has_output_amount = sw->has_output_amount;

    idx_pump_amm_accounts acc;
    if (sw->has_pool && find_pump_amm_accounts(b->tx, &sw->pool, &acc)) {
        bool input_is_base =
            sw->has_input_account &&
            idx_pubkey_equal(&sw->input_account, &acc.user_base_account);
        const idx_pubkey *input_mint =
            input_is_base ? &acc.base_mint : &acc.quote_mint;
        const idx_pubkey *output_mint =
            input_is_base ? &acc.quote_mint : &acc.base_mint;
        set_side_mint(b->tx, input_mint, &row->input_mint, &row->has_input_mint,
                      &row->input_decimals, &row->has_input_decimals);
        set_side_mint(b->tx, output_mint, &row->output_mint,
                      &row->has_output_mint, &row->output_decimals,
                      &row->has_output_decimals);
        return;
    }

    /* No instruction to pair with — a truncated inner list, say. Fall back to
     * the trader's accounts, which resolves the side that is not a temporary
     * wrapped-SOL account. */
    if (sw->has_input_account) {
        resolve_side_from_account(b->tx, &sw->input_account, &row->input_mint,
                                  &row->has_input_mint, &row->input_decimals,
                                  &row->has_input_decimals);
    }
    if (sw->has_output_account) {
        resolve_side_from_account(b->tx, &sw->output_account, &row->output_mint,
                                  &row->has_output_mint, &row->output_decimals,
                                  &row->has_output_decimals);
    }
}

/* Fills both mints, from the accounts the venue named or from the mints it
 * stated outright (Raydium CLMM's swapV2). */
static void resolve_both_mints(builder *b, const idx_swap *sw,
                               idx_swap_row *row) {
    if (sw->has_input_mint) {
        row->input_mint = sw->input_mint;
        row->has_input_mint = true;
        row->has_input_decimals =
            decimals_of_mint(b->tx, &sw->input_mint, &row->input_decimals);
    } else if (sw->has_input_account) {
        resolve_side_from_account(b->tx, &sw->input_account, &row->input_mint,
                                  &row->has_input_mint, &row->input_decimals,
                                  &row->has_input_decimals);
    }
    if (sw->has_output_mint) {
        row->output_mint = sw->output_mint;
        row->has_output_mint = true;
        row->has_output_decimals =
            decimals_of_mint(b->tx, &sw->output_mint, &row->output_decimals);
    } else if (sw->has_output_account) {
        resolve_side_from_account(b->tx, &sw->output_account, &row->output_mint,
                                  &row->has_output_mint, &row->output_decimals,
                                  &row->has_output_decimals);
    }
}

/*
 * Raydium AMM v4: the instruction fixes one amount, ray_log states both (D9's
 * preferred source), and the vault deltas are the fallback when the log was
 * truncated. The vaults are matched to the input and output mints rather than
 * taken by position, because v4 orders them coin/pc, not input/output.
 */
static void normalize_raydium_amm(builder *b, const idx_swap *sw,
                                  idx_swap_row *row) {
    row->pool = sw->pool;
    row->has_pool = sw->has_pool;
    row->user = sw->user;
    row->has_user = sw->has_user;
    resolve_both_mints(b, sw, row);

    /* Whichever side the instruction fixed, for the cross-check and as the
     * fallback authority for that side. */
    uint64_t fixed = sw->has_input_amount ? sw->input_amount : sw->output_amount;
    bool fixed_is_input = sw->has_input_amount;

    if (b->raylog_next < b->raylog_count) {
        const idx_raydium_swap_log *log = &b->raylogs[b->raylog_next++];
        uint64_t log_fixed = fixed_is_input ? log->amount_in : log->amount_out;
        if (fixed == 0 || log_fixed == fixed) {
            row->source = IDX_AMOUNT_RAYLOG;
            row->input_amount = log->amount_in;
            row->has_input_amount = true;
            row->output_amount = log->amount_out;
            row->has_output_amount = true;
            return;
        }
        /* The pairing disagreed with the amount the instruction stated, so it
         * is not trusted; fall through to the deltas. */
    }

    row->source = IDX_AMOUNT_DELTA;
    row->input_amount = sw->input_amount;
    row->has_input_amount = sw->has_input_amount;
    row->output_amount = sw->output_amount;
    row->has_output_amount = sw->has_output_amount;
    if (!row->has_input_amount && row->has_input_mint && sw->has_pool_accounts) {
        row->has_input_amount = amount_from_vault_of_mint(
            b->tx, &sw->pool_account_a, &sw->pool_account_b, &row->input_mint,
            &row->input_amount);
    }
    if (!row->has_output_amount && row->has_output_mint &&
        sw->has_pool_accounts) {
        row->has_output_amount = amount_from_vault_of_mint(
            b->tx, &sw->pool_account_a, &sw->pool_account_b, &row->output_mint,
            &row->output_amount);
    }
    (void)fixed;
}

/*
 * Raydium CLMM: the instruction fixes one amount and names its vaults by
 * position (a is the input vault, b the output), so the missing side is the
 * matching vault's delta.
 */
static void normalize_raydium_clmm(builder *b, const idx_swap *sw,
                                   idx_swap_row *row) {
    row->pool = sw->pool;
    row->has_pool = sw->has_pool;
    row->user = sw->user;
    row->has_user = sw->has_user;
    row->source = IDX_AMOUNT_DELTA;
    resolve_both_mints(b, sw, row);

    row->input_amount = sw->input_amount;
    row->has_input_amount = sw->has_input_amount;
    row->output_amount = sw->output_amount;
    row->has_output_amount = sw->has_output_amount;
    if (!sw->has_pool_accounts) {
        return;
    }
    uint64_t magnitude = 0;
    bool increased = false;
    if (!row->has_input_amount &&
        vault_delta(b->tx, &sw->pool_account_a, &magnitude, &increased)) {
        row->input_amount = magnitude;
        row->has_input_amount = true;
    }
    if (!row->has_output_amount &&
        vault_delta(b->tx, &sw->pool_account_b, &magnitude, &increased)) {
        row->output_amount = magnitude;
        row->has_output_amount = true;
    }
}

/* --------------------------------------------------------- route netting -- */

/* Adds one leg to the per-mint totals the route is netted from. */
static void add_route_leg(builder *b, const idx_swap *sw) {
    b->saw_route = true;
    const struct {
        const idx_pubkey *mint;
        uint64_t amount;
        bool is_input;
    } sides[2] = {
        {&sw->input_mint, sw->input_amount, true},
        {&sw->output_mint, sw->output_amount, false},
    };
    for (size_t s = 0; s < 2; s++) {
        route_mint *slot = NULL;
        for (size_t i = 0; i < b->net_count; i++) {
            if (idx_pubkey_equal(&b->net[i].mint, sides[s].mint)) {
                slot = &b->net[i];
                break;
            }
        }
        if (slot == NULL) {
            if (b->net_count >= MAX_ROUTE_MINTS) {
                b->net_overflow = true;
                return;
            }
            slot = &b->net[b->net_count++];
            slot->mint = *sides[s].mint;
            slot->in_sum = 0;
            slot->out_sum = 0;
        }
        if (sides[s].is_input) {
            slot->in_sum += sides[s].amount;
        } else {
            slot->out_sum += sides[s].amount;
        }
    }
}

/*
 * The completed-trade row a route nets to: the mint it consumed on net is what
 * the wallet paid, the mint it produced on net is what it got, and the mints
 * that only passed through cancel. A route that nets nothing on both ends is
 * cyclic — a self-trade — and yields no row (D8).
 */
static void flush_route(builder *b) {
    if (!b->saw_route || b->net_overflow || b->count >= b->capacity) {
        return;
    }

    const route_mint *input = NULL;
    const route_mint *output = NULL;
    uint64_t input_net = 0;
    uint64_t output_net = 0;
    for (size_t i = 0; i < b->net_count; i++) {
        const route_mint *m = &b->net[i];
        if (m->in_sum > m->out_sum) {
            uint64_t net = m->in_sum - m->out_sum;
            if (net > input_net) {
                input_net = net;
                input = m;
            }
        } else if (m->out_sum > m->in_sum) {
            uint64_t net = m->out_sum - m->in_sum;
            if (net > output_net) {
                output_net = net;
                output = m;
            }
        }
    }
    if (input == NULL || output == NULL) {
        return;
    }

    idx_swap_row *row = &b->rows[b->count++];
    memset(row, 0, sizeof(*row));
    row->kind = IDX_SWAP_AGGREGATED;
    row->venue = IDX_VENUE_JUPITER;
    row->source = IDX_AMOUNT_EVENT;
    row->instruction_index = b->route_top_index;
    /* The wallet that sent the transaction is the trader. */
    if (b->tx->account_count > 0) {
        row->user = b->tx->accounts[0].pubkey;
        row->has_user = true;
    }
    row->input_mint = input->mint;
    row->has_input_mint = true;
    row->input_amount = input_net;
    row->has_input_amount = true;
    row->has_input_decimals =
        decimals_of_mint(b->tx, &input->mint, &row->input_decimals);
    row->output_mint = output->mint;
    row->has_output_mint = true;
    row->output_amount = output_net;
    row->has_output_amount = true;
    row->has_output_decimals =
        decimals_of_mint(b->tx, &output->mint, &row->output_decimals);
}

/* ---------------------------------------------------------------- walk -- */

static idx_status normalize_instruction(builder *b, const idx_instruction *ix,
                                        idx_error *err) {
    idx_swap sw;
    idx_status status = idx_swap_decode(b->tx, ix, &sw, err);
    if (status == IDX_ERR_NOT_FOUND) {
        return IDX_OK; /* not a swap: almost every instruction */
    }
    if (status != IDX_OK) {
        return status; /* a recognised payload this could not read */
    }

    if (!idx_venue_is_pool(sw.venue)) {
        /* Jupiter: a route leg, netted rather than emitted (D8). */
        if (!b->saw_route) {
            b->route_top_index = b->instruction_index;
        }
        add_route_leg(b, &sw);
        return IDX_OK;
    }

    idx_swap_row *row = builder_row(b, sw.venue, IDX_AMOUNT_EVENT);
    if (row == NULL) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL,
                        "more swaps than instructions to hold them");
    }
    switch (sw.venue) {
    case IDX_VENUE_PUMP_CURVE:
        normalize_pump_curve(b, &sw, row);
        break;
    case IDX_VENUE_PUMP_AMM:
        normalize_pump_amm(b, &sw, row);
        break;
    case IDX_VENUE_RAYDIUM_AMM_V4:
        normalize_raydium_amm(b, &sw, row);
        break;
    case IDX_VENUE_RAYDIUM_CLMM:
    case IDX_VENUE_RAYDIUM_CPMM:
        normalize_raydium_clmm(b, &sw, row);
        break;
    case IDX_VENUE_JUPITER:
    case IDX_VENUE_NONE:
        break; /* not pools; handled above or impossible here */
    }
    b->count++;
    return IDX_OK;
}

/* The inner instructions of top-level instruction `index`, or NULL. */
static const idx_inner_instructions *inner_group(const idx_transaction *tx,
                                                 size_t index) {
    for (size_t i = 0; i < tx->inner_instruction_count; i++) {
        if (tx->inner_instructions[i].index == index) {
            return &tx->inner_instructions[i];
        }
    }
    return NULL;
}

static size_t instruction_total(const idx_transaction *tx) {
    size_t total = tx->instruction_count;
    for (size_t i = 0; i < tx->inner_instruction_count; i++) {
        total += tx->inner_instructions[i].instruction_count;
    }
    return total;
}

idx_status idx_swap_normalize(const idx_transaction *tx, idx_arena *arena,
                              const idx_swap_row **out, size_t *out_count,
                              idx_error *err) {
    if (tx == NULL || arena == NULL || out == NULL || out_count == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, arena, out and out_count must not be NULL");
    }

    *out = NULL;
    *out_count = 0;
    if (!tx->has_meta || !tx->success || tx->accounts == NULL) {
        return IDX_OK;
    }
    size_t instructions = instruction_total(tx);
    if (instructions == 0) {
        return IDX_OK;
    }

    /* One pool row per instruction, plus one aggregated route row. */
    void *raw = NULL;
    IDX_TRY(idx_arena_calloc(arena, instructions + 1, sizeof(idx_swap_row),
                             &raw, err));

    builder b;
    memset(&b, 0, sizeof(b));
    b.tx = tx;
    b.rows = raw;
    b.capacity = instructions + 1;
    IDX_TRY(collect_raylogs(tx, arena, &b.raylogs, &b.raylog_count, err));

    for (size_t i = 0; i < tx->instruction_count; i++) {
        b.instruction_index = (uint16_t)i;
        b.inner_index = 0;
        b.inner = false;
        IDX_TRY(normalize_instruction(&b, &tx->instructions[i], err));

        const idx_inner_instructions *group = inner_group(tx, i);
        if (group == NULL) {
            continue;
        }
        b.inner = true;
        for (size_t j = 0; j < group->instruction_count; j++) {
            b.inner_index = (uint16_t)j;
            IDX_TRY(normalize_instruction(&b, &group->instructions[j], err));
        }
    }

    flush_route(&b);

    if (b.count == 0) {
        return IDX_OK;
    }
    *out = b.rows;
    *out_count = b.count;
    return IDX_OK;
}
