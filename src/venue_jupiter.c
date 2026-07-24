#include "venue_jupiter.h"

#include <string.h>

#include "bytes.h"

/* sha256("event:SwapEvent")[..8]. */
static const uint8_t SWAP_EVENT[8] = {0x40, 0xc6, 0xcd, 0xe8,
                                      0x26, 0x08, 0x71, 0xe2};

/*
 * SwapEvent, all of it:
 *
 *   amm           pubkey   dropped, see the header
 *   input_mint    pubkey
 *   input_amount  u64
 *   output_mint   pubkey
 *   output_amount u64
 *
 * Unlike pump's events this one has a fixed shape and no trailing string, but
 * it is read field by field for the same reason: what follows a field this
 * indexer reads is not this indexer's business.
 */
static idx_status decode_swap_event(idx_slice fields, idx_swap *out,
                                    idx_error *err) {
    idx_cursor cursor;
    idx_cursor_init(&cursor, fields);
    IDX_TRY(idx_cursor_skip(&cursor, IDX_PUBKEY_LEN, err)); /* amm */
    IDX_TRY(idx_cursor_copy(&cursor, out->input_mint.bytes, IDX_PUBKEY_LEN,
                            err));
    IDX_TRY(idx_cursor_u64le(&cursor, &out->input_amount, err));
    IDX_TRY(idx_cursor_copy(&cursor, out->output_mint.bytes, IDX_PUBKEY_LEN,
                            err));
    IDX_TRY(idx_cursor_u64le(&cursor, &out->output_amount, err));
    out->has_input_mint = true;
    out->has_output_mint = true;
    out->has_input_amount = true;
    out->has_output_amount = true;
    return IDX_OK;
}

idx_status idx_venue_jupiter_decode(const idx_transaction *tx,
                                    const idx_instruction *ix, idx_swap *out,
                                    idx_error *err) {
    if (tx == NULL || ix == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, ix and out must not be NULL");
    }
    memset(out, 0, sizeof(*out));
    out->venue = IDX_VENUE_JUPITER;
    out->from_event = true;

    idx_slice payload;
    idx_slice fields;
    if (!idx_anchor_event_payload(ix->data, &payload) ||
        !idx_anchor_event_is(payload, SWAP_EVENT, &fields)) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "not a jupiter swap event");
    }
    return decode_swap_event(fields, out, err);
}
