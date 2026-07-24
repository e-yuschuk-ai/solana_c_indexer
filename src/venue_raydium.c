#include "venue_raydium.h"

#include <string.h>

#include "bytes.h"

/* AMM v4 discriminants: one byte, as the program packs them. */
#define RAYDIUM_AMM_IX_SWAP_BASE_IN 9
#define RAYDIUM_AMM_IX_SWAP_BASE_OUT 11

/*
 * AMM v4 account layouts. The trailing three are the trader's — source,
 * destination, owner — and are addressed from the end, because what varies is
 * in the middle: a pool with no OpenBook market pads the serum accounts, and
 * `amm_target_orders` is present in one shape and absent in the other.
 *
 *   17 accounts   token_program, amm, authority, open_orders, coin_vault,
 *                 pc_vault, then the serum block, then the trader's three
 *   18 accounts   the same with amm_target_orders at index 4, which pushes
 *                 the vaults one along
 */
#define RAYDIUM_AMM_POOL_INDEX 1
#define RAYDIUM_AMM_ACCOUNTS_SHORT 17
#define RAYDIUM_AMM_ACCOUNTS_LONG 18
#define RAYDIUM_AMM_VAULT_INDEX_SHORT 4
#define RAYDIUM_AMM_VAULT_INDEX_LONG 5
#define RAYDIUM_AMM_USER_TAIL 3

/* CLMM instruction discriminators, sha256("global:<name>")[..8]. */
static const uint8_t CLMM_SWAP[8] = {0xf8, 0xc6, 0x9e, 0x91,
                                     0xe1, 0x75, 0x87, 0xc8};
static const uint8_t CLMM_SWAP_V2[8] = {0x2b, 0x04, 0xed, 0x0b,
                                        0x1a, 0xc9, 0x1e, 0x62};

/*
 * CLMM account layout, shared by `swap` and `swapV2` up to the token program:
 *
 *   0 payer   1 amm_config   2 pool_state   3 input_token_account
 *   4 output_token_account   5 input_vault  6 output_vault
 *
 * `swapV2` continues with the 2022-aware programs and then names the two vault
 * mints at 11 and 12, which is the only Raydium shape that states a mint.
 */
#define CLMM_PAYER_INDEX 0
#define CLMM_POOL_INDEX 2
#define CLMM_USER_INPUT_INDEX 3
#define CLMM_USER_OUTPUT_INDEX 4
#define CLMM_INPUT_VAULT_INDEX 5
#define CLMM_OUTPUT_VAULT_INDEX 6
#define CLMM_V2_INPUT_MINT_INDEX 11
#define CLMM_V2_OUTPUT_MINT_INDEX 12
#define CLMM_MIN_ACCOUNTS 7
#define CLMM_V2_MIN_ACCOUNTS 13

/* Resolves an operand, or reports the instruction too short for this venue. */
static idx_status operand(const idx_transaction *tx, const idx_instruction *ix,
                          size_t position, const char *what,
                          const idx_pubkey **out, idx_error *err) {
    const idx_pubkey *account = idx_instruction_account(tx, ix, position);
    if (account == NULL) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "raydium swap needs %s at account %zu, instruction has "
                        "%zu",
                        what, position, ix->account_count);
    }
    *out = account;
    return IDX_OK;
}

static idx_status decode_amm_v4(const idx_transaction *tx,
                                const idx_instruction *ix, idx_swap *out,
                                idx_error *err) {
    idx_cursor cursor;
    idx_cursor_init(&cursor, ix->data);
    uint8_t discriminant = 0;
    IDX_TRY(idx_cursor_u8(&cursor, &discriminant, err));
    if (discriminant != RAYDIUM_AMM_IX_SWAP_BASE_IN &&
        discriminant != RAYDIUM_AMM_IX_SWAP_BASE_OUT) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "raydium amm instruction %u is "
                        "not a swap", (unsigned)discriminant);
    }

    /* An unfamiliar shape is skipped: the vault indices below would otherwise
     * name whatever happened to sit there. */
    size_t vault_index = 0;
    if (ix->account_count == RAYDIUM_AMM_ACCOUNTS_SHORT) {
        vault_index = RAYDIUM_AMM_VAULT_INDEX_SHORT;
    } else if (ix->account_count == RAYDIUM_AMM_ACCOUNTS_LONG) {
        vault_index = RAYDIUM_AMM_VAULT_INDEX_LONG;
    } else {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND,
                        "raydium amm swap over %zu accounts is a layout this "
                        "decoder has not seen",
                        ix->account_count);
    }

    /*
     * SwapBaseIn fixes what the trader pays and leaves a floor under what they
     * get; SwapBaseOut does the reverse. Only the fixed one is recorded — the
     * limit is what was asked for, not what happened.
     */
    uint64_t first = 0;
    uint64_t second = 0;
    IDX_TRY(idx_cursor_u64le(&cursor, &first, err));
    IDX_TRY(idx_cursor_u64le(&cursor, &second, err));
    if (discriminant == RAYDIUM_AMM_IX_SWAP_BASE_IN) {
        out->input_amount = first;
        out->has_input_amount = true;
    } else {
        out->output_amount = second;
        out->has_output_amount = true;
    }

    const idx_pubkey *account = NULL;
    IDX_TRY(operand(tx, ix, RAYDIUM_AMM_POOL_INDEX, "the pool", &account, err));
    out->pool = *account;
    out->has_pool = true;

    IDX_TRY(operand(tx, ix, vault_index, "a vault", &account, err));
    out->pool_account_a = *account;
    IDX_TRY(operand(tx, ix, vault_index + 1, "a vault", &account, err));
    out->pool_account_b = *account;
    out->has_pool_accounts = true;

    size_t tail = ix->account_count - RAYDIUM_AMM_USER_TAIL;
    IDX_TRY(operand(tx, ix, tail, "the source", &account, err));
    out->input_account = *account;
    out->has_input_account = true;
    IDX_TRY(operand(tx, ix, tail + 1, "the destination", &account, err));
    out->output_account = *account;
    out->has_output_account = true;
    IDX_TRY(operand(tx, ix, tail + 2, "the trader", &account, err));
    out->user = *account;
    out->has_user = true;
    return IDX_OK;
}

static idx_status decode_clmm(const idx_transaction *tx,
                              const idx_instruction *ix, idx_slice payload,
                              bool is_v2, idx_swap *out, idx_error *err) {
    size_t needed = is_v2 ? CLMM_V2_MIN_ACCOUNTS : CLMM_MIN_ACCOUNTS;
    if (ix->account_count < needed) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND,
                        "raydium clmm swap over %zu accounts is a layout this "
                        "decoder has not seen",
                        ix->account_count);
    }

    /*
     * amount, other_amount_threshold, sqrt_price_limit_x64 (u128), and the
     * flag that says which of the two the amount is. The threshold is the
     * caller's limit and is not recorded.
     */
    idx_cursor cursor;
    idx_cursor_init(&cursor, payload);
    uint64_t amount = 0;
    uint64_t threshold = 0;
    uint8_t is_base_input = 0;
    IDX_TRY(idx_cursor_u64le(&cursor, &amount, err));
    IDX_TRY(idx_cursor_u64le(&cursor, &threshold, err));
    IDX_TRY(idx_cursor_skip(&cursor, 16, err)); /* sqrt_price_limit_x64 */
    IDX_TRY(idx_cursor_u8(&cursor, &is_base_input, err));
    if (is_base_input != 0) {
        out->input_amount = amount;
        out->has_input_amount = true;
    } else {
        out->output_amount = amount;
        out->has_output_amount = true;
    }

    const idx_pubkey *account = NULL;
    IDX_TRY(operand(tx, ix, CLMM_POOL_INDEX, "the pool", &account, err));
    out->pool = *account;
    out->has_pool = true;
    IDX_TRY(operand(tx, ix, CLMM_PAYER_INDEX, "the trader", &account, err));
    out->user = *account;
    out->has_user = true;
    IDX_TRY(operand(tx, ix, CLMM_USER_INPUT_INDEX, "the source", &account,
                    err));
    out->input_account = *account;
    out->has_input_account = true;
    IDX_TRY(operand(tx, ix, CLMM_USER_OUTPUT_INDEX, "the destination", &account,
                    err));
    out->output_account = *account;
    out->has_output_account = true;
    IDX_TRY(operand(tx, ix, CLMM_INPUT_VAULT_INDEX, "a vault", &account, err));
    out->pool_account_a = *account;
    IDX_TRY(operand(tx, ix, CLMM_OUTPUT_VAULT_INDEX, "a vault", &account, err));
    out->pool_account_b = *account;
    out->has_pool_accounts = true;

    if (!is_v2) {
        return IDX_OK;
    }
    IDX_TRY(operand(tx, ix, CLMM_V2_INPUT_MINT_INDEX, "the input mint",
                    &account, err));
    out->input_mint = *account;
    out->has_input_mint = true;
    IDX_TRY(operand(tx, ix, CLMM_V2_OUTPUT_MINT_INDEX, "the output mint",
                    &account, err));
    out->output_mint = *account;
    out->has_output_mint = true;
    return IDX_OK;
}

idx_status idx_venue_raydium_decode(const idx_transaction *tx,
                                    const idx_instruction *ix, idx_venue venue,
                                    idx_swap *out, idx_error *err) {
    if (tx == NULL || ix == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, ix and out must not be NULL");
    }
    memset(out, 0, sizeof(*out));
    out->venue = venue;

    if (venue == IDX_VENUE_RAYDIUM_AMM_V4) {
        return decode_amm_v4(tx, ix, out, err);
    }
    if (venue != IDX_VENUE_RAYDIUM_CLMM) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND,
                        "%s has no swap decoder yet", idx_venue_name(venue));
    }

    if (ix->data.len < 8) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "not a clmm swap");
    }
    idx_slice payload = idx_slice_sub(ix->data, 8, ix->data.len - 8);
    if (memcmp(ix->data.data, CLMM_SWAP, 8) == 0) {
        return decode_clmm(tx, ix, payload, false, out, err);
    }
    if (memcmp(ix->data.data, CLMM_SWAP_V2, 8) == 0) {
        return decode_clmm(tx, ix, payload, true, out, err);
    }
    return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "not a clmm swap");
}
