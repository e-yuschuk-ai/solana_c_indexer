#include "venue_pump.h"

#include <string.h>

#include "bytes.h"

/* Event discriminators, sha256("event:<Name>")[..8]. */
static const uint8_t TRADE_EVENT[8] = {0xbd, 0xdb, 0x7f, 0xd3,
                                       0x4e, 0xe6, 0x61, 0xee};
static const uint8_t BUY_EVENT[8] = {0x67, 0xf4, 0x52, 0x1f,
                                     0x2c, 0xf5, 0x77, 0x77};
static const uint8_t SELL_EVENT[8] = {0x3e, 0x2f, 0x37, 0x0a,
                                      0xa5, 0x03, 0xdc, 0x2a};
static const uint8_t CREATE_EVENT[8] = {0x1b, 0x72, 0xa9, 0x4d,
                                        0xde, 0xeb, 0x63, 0x76};

/*
 * The bonding curve's TradeEvent, of which this reads the leading fields:
 *
 *   mint         pubkey    the token traded; the curve holds the SOL side
 *   sol_amount   u64       lamports that moved
 *   token_amount u64       raw token units that moved
 *   is_buy       u8        true when the trader bought the token
 *   user         pubkey    the trader
 *
 * What follows — timestamp, the virtual and real reserves, the fee breakdown,
 * and a trailing string naming the instruction — is left alone. Those fields
 * have been appended to twice and none of them is a swap.
 *
 * The curve trades native lamports, not wrapped SOL, and the SOL side is
 * reported as the wrapped mint anyway: it is the identity a quote set can
 * match, and D5 already treats SOL and WSOL as one quote.
 */
static idx_status decode_trade_event(idx_slice fields, idx_swap *out,
                                     idx_error *err) {
    idx_cursor cursor;
    idx_cursor_init(&cursor, fields);

    idx_pubkey mint;
    uint64_t sol_amount = 0;
    uint64_t token_amount = 0;
    uint8_t is_buy = 0;
    idx_pubkey user;
    IDX_TRY(idx_cursor_copy(&cursor, mint.bytes, IDX_PUBKEY_LEN, err));
    IDX_TRY(idx_cursor_u64le(&cursor, &sol_amount, err));
    IDX_TRY(idx_cursor_u64le(&cursor, &token_amount, err));
    IDX_TRY(idx_cursor_u8(&cursor, &is_buy, err));
    IDX_TRY(idx_cursor_copy(&cursor, user.bytes, IDX_PUBKEY_LEN, err));

    out->user = user;
    out->has_user = true;
    if (is_buy != 0) {
        out->input_mint = IDX_MINT_WSOL;
        out->input_amount = sol_amount;
        out->output_mint = mint;
        out->output_amount = token_amount;
    } else {
        out->input_mint = mint;
        out->input_amount = token_amount;
        out->output_mint = IDX_MINT_WSOL;
        out->output_amount = sol_amount;
    }
    out->has_input_mint = true;
    out->has_output_mint = true;
    out->has_input_amount = true;
    out->has_output_amount = true;
    return IDX_OK;
}

/*
 * PumpSwap's BuyEvent and SellEvent, which share a field order and differ only
 * in what each name means. Read here:
 *
 *   +0    timestamp                     i64
 *   +8    base_amount_out / _in         u64   the token side
 *   ...   the reserves and the requested limit
 *   +104  user_quote_amount_in / _out   u64   what the trader paid or got
 *   +112  pool                          pubkey
 *   +144  user                          pubkey
 *   +176  user_base_token_account       pubkey
 *   +208  user_quote_token_account      pubkey
 *
 * The quote amount taken is the trader's, not the pool's: it is what left or
 * reached the wallet, it matches what an aggregator reports for the same leg,
 * and the fee split that separates the two belongs to the pool's accounting
 * rather than to the trade.
 *
 * Neither event names the mints — base and quote are properties of the pool,
 * not of the trade — so the trader's two token accounts are carried instead
 * and the mints come from the block's token balances for them.
 */
#define PUMP_AMM_BASE_AMOUNT_OFFSET 8
#define PUMP_AMM_USER_QUOTE_AMOUNT_OFFSET 104
#define PUMP_AMM_POOL_OFFSET 112
#define PUMP_AMM_USER_OFFSET 144
#define PUMP_AMM_USER_BASE_ACCOUNT_OFFSET 176
#define PUMP_AMM_USER_QUOTE_ACCOUNT_OFFSET 208
#define PUMP_AMM_EVENT_PREFIX_LEN 240

static idx_status decode_amm_event(idx_slice fields, bool is_buy, idx_swap *out,
                                   idx_error *err) {
    if (fields.len < PUMP_AMM_EVENT_PREFIX_LEN) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "pump amm %s event is %zu bytes, needs %d",
                        is_buy ? "buy" : "sell", fields.len,
                        PUMP_AMM_EVENT_PREFIX_LEN);
    }

    idx_cursor cursor;
    uint64_t base_amount = 0;
    uint64_t quote_amount = 0;
    idx_cursor_init(&cursor, idx_slice_sub(fields, PUMP_AMM_BASE_AMOUNT_OFFSET,
                                           sizeof(uint64_t)));
    IDX_TRY(idx_cursor_u64le(&cursor, &base_amount, err));
    idx_cursor_init(&cursor,
                    idx_slice_sub(fields, PUMP_AMM_USER_QUOTE_AMOUNT_OFFSET,
                                  sizeof(uint64_t)));
    IDX_TRY(idx_cursor_u64le(&cursor, &quote_amount, err));

    memcpy(out->pool.bytes, fields.data + PUMP_AMM_POOL_OFFSET, IDX_PUBKEY_LEN);
    out->has_pool = true;
    memcpy(out->user.bytes, fields.data + PUMP_AMM_USER_OFFSET, IDX_PUBKEY_LEN);
    out->has_user = true;

    idx_pubkey base_account;
    idx_pubkey quote_account;
    memcpy(base_account.bytes, fields.data + PUMP_AMM_USER_BASE_ACCOUNT_OFFSET,
           IDX_PUBKEY_LEN);
    memcpy(quote_account.bytes,
           fields.data + PUMP_AMM_USER_QUOTE_ACCOUNT_OFFSET, IDX_PUBKEY_LEN);

    if (is_buy) {
        out->input_account = quote_account;
        out->input_amount = quote_amount;
        out->output_account = base_account;
        out->output_amount = base_amount;
    } else {
        out->input_account = base_account;
        out->input_amount = base_amount;
        out->output_account = quote_account;
        out->output_amount = quote_amount;
    }
    out->has_input_account = true;
    out->has_output_account = true;
    out->has_input_amount = true;
    out->has_output_amount = true;
    return IDX_OK;
}

/*
 * PumpSwap swap instruction discriminators, sha256("global:<name>")[..8]. The
 * variants share one account order for the accounts this reads, so they are
 * treated alike here: `buy_exact_quote_in` is a `buy` that fixes the quote it
 * spends rather than the base it wants, and names the same pool, mints and
 * vaults in the same positions.
 */
static const uint8_t AMM_BUY[8] = {0x66, 0x06, 0x3d, 0x12,
                                   0x01, 0xda, 0xeb, 0xea};
static const uint8_t AMM_SELL[8] = {0x33, 0xe6, 0x85, 0xa4,
                                    0x01, 0x7f, 0x83, 0xad};
static const uint8_t AMM_BUY_EXACT_QUOTE_IN[8] = {0xc6, 0x2e, 0x15, 0x52,
                                                  0xb4, 0xd9, 0xe8, 0x70};

/* The swap variants share this account order; only the leading accounts, which
 * is all this reads, are asserted stable. */
#define AMM_IX_POOL 0
#define AMM_IX_BASE_MINT 3
#define AMM_IX_QUOTE_MINT 4
#define AMM_IX_USER_BASE_ACCOUNT 5
#define AMM_IX_POOL_BASE_VAULT 7
#define AMM_IX_POOL_QUOTE_VAULT 8
#define AMM_IX_MIN_ACCOUNTS 9

bool idx_venue_pump_amm_accounts(const idx_transaction *tx,
                                 const idx_instruction *ix,
                                 idx_pump_amm_accounts *out) {
    if (tx == NULL || ix == NULL || out == NULL || ix->data.len < 8) {
        return false;
    }
    if (memcmp(ix->data.data, AMM_BUY, 8) != 0 &&
        memcmp(ix->data.data, AMM_SELL, 8) != 0 &&
        memcmp(ix->data.data, AMM_BUY_EXACT_QUOTE_IN, 8) != 0) {
        return false;
    }
    if (ix->account_count < AMM_IX_MIN_ACCOUNTS) {
        return false;
    }
    out->pool = *idx_instruction_account(tx, ix, AMM_IX_POOL);
    out->base_mint = *idx_instruction_account(tx, ix, AMM_IX_BASE_MINT);
    out->quote_mint = *idx_instruction_account(tx, ix, AMM_IX_QUOTE_MINT);
    out->user_base_account =
        *idx_instruction_account(tx, ix, AMM_IX_USER_BASE_ACCOUNT);
    out->pool_base_vault =
        *idx_instruction_account(tx, ix, AMM_IX_POOL_BASE_VAULT);
    out->pool_quote_vault =
        *idx_instruction_account(tx, ix, AMM_IX_POOL_QUOTE_VAULT);
    return true;
}

/* Reads one Borsh string as a borrowed slice: a u32 little-endian length and
 * that many bytes, which stay pointing into the instruction's data. */
static idx_status read_borsh_string(idx_cursor *cursor, idx_slice *out,
                                    idx_error *err) {
    uint32_t len = 0;
    IDX_TRY(idx_cursor_u32le(cursor, &len, err));
    return idx_cursor_take(cursor, len, out, err);
}

/*
 * The bonding curve's CreateEvent, of which this reads the leading fields:
 *
 *   name          string    the token's display name, symbol and metadata URI,
 *   symbol        string    which the token registry keeps and the pool one
 *   uri           string    ignores
 *   mint          pubkey    the token; the curve's identity, since there is one
 *                           curve per mint and swaps key the curve by it
 *   bonding_curve pubkey    the curve account (not used here)
 *   user          pubkey    the wallet that created it
 *
 * What follows — the creator field pump added later, the timestamp, the initial
 * reserves — is left alone, the same way the trade events are read by prefix.
 * Verified against a mainnet CreateEvent (mint …pump, name "United States Water
 * Reserve", symbol "USWR"): the three strings then the three pubkeys, in this
 * order. `user` is taken as the creator — the account that signed the create,
 * the deployer a terminal shows — since unlike the appended `creator` field it
 * is always present.
 */
typedef struct {
    idx_slice name;
    idx_slice symbol;
    idx_slice uri;
    idx_pubkey mint;
    idx_pubkey bonding_curve;
    idx_pubkey user;
} pump_create_event;

static idx_status parse_create_event(idx_slice ix_data, pump_create_event *out,
                                     idx_error *err) {
    idx_slice payload;
    if (!idx_anchor_event_payload(ix_data, &payload)) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "not an anchor event");
    }
    idx_slice fields;
    if (!idx_anchor_event_is(payload, CREATE_EVENT, &fields)) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "not a create event");
    }

    idx_cursor cursor;
    idx_cursor_init(&cursor, fields);
    IDX_TRY(read_borsh_string(&cursor, &out->name, err));
    IDX_TRY(read_borsh_string(&cursor, &out->symbol, err));
    IDX_TRY(read_borsh_string(&cursor, &out->uri, err));
    IDX_TRY(idx_cursor_copy(&cursor, out->mint.bytes, IDX_PUBKEY_LEN, err));
    IDX_TRY(idx_cursor_copy(&cursor, out->bonding_curve.bytes, IDX_PUBKEY_LEN,
                            err));
    IDX_TRY(idx_cursor_copy(&cursor, out->user.bytes, IDX_PUBKEY_LEN, err));
    return IDX_OK;
}

idx_status idx_venue_pump_creation(const idx_transaction *tx,
                                   const idx_instruction *ix,
                                   idx_pool_creation *out, idx_error *err) {
    if (tx == NULL || ix == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, ix and out must not be NULL");
    }
    memset(out, 0, sizeof(*out));
    out->venue = IDX_VENUE_PUMP_CURVE;

    pump_create_event event;
    IDX_TRY(parse_create_event(ix->data, &event, err));
    out->pool = event.mint;
    out->has_pool = true;
    out->creator = event.user;
    out->has_creator = true;
    return IDX_OK;
}

idx_status idx_venue_pump_metadata(const idx_transaction *tx,
                                   const idx_instruction *ix,
                                   idx_token_metadata *out, idx_error *err) {
    if (tx == NULL || ix == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, ix and out must not be NULL");
    }
    memset(out, 0, sizeof(*out));

    pump_create_event event;
    IDX_TRY(parse_create_event(ix->data, &event, err));
    out->mint = event.mint;
    out->has_mint = true;
    out->name = event.name;
    out->has_name = !idx_slice_is_empty(event.name);
    out->symbol = event.symbol;
    out->has_symbol = !idx_slice_is_empty(event.symbol);
    out->uri = event.uri;
    out->has_uri = !idx_slice_is_empty(event.uri);
    return IDX_OK;
}

idx_status idx_venue_pump_decode(const idx_transaction *tx,
                                 const idx_instruction *ix, idx_venue venue,
                                 idx_swap *out, idx_error *err) {
    if (tx == NULL || ix == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, ix and out must not be NULL");
    }
    memset(out, 0, sizeof(*out));
    out->venue = venue;
    out->from_event = true;

    idx_slice payload;
    if (!idx_anchor_event_payload(ix->data, &payload)) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "not an anchor event");
    }

    idx_slice fields;
    if (venue == IDX_VENUE_PUMP_CURVE) {
        if (!idx_anchor_event_is(payload, TRADE_EVENT, &fields)) {
            return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "not a curve trade");
        }
        return decode_trade_event(fields, out, err);
    }
    if (idx_anchor_event_is(payload, BUY_EVENT, &fields)) {
        return decode_amm_event(fields, true, out, err);
    }
    if (idx_anchor_event_is(payload, SELL_EVENT, &fields)) {
        return decode_amm_event(fields, false, out, err);
    }
    return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "not a pump amm trade");
}
