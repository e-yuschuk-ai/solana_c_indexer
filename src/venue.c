#include "venue.h"

#include <string.h>

#include "venue_jupiter.h"
#include "venue_pump.h"
#include "venue_raydium.h"

/*
 * Venue program ids, as raw bytes for the same reason the built-in ones in
 * types.c are: no decoding at startup and a direct comparison. The base58 form
 * of each is in the comment above it, and test_venue asserts the two agree.
 */

/* 6EF8rrecthR5Dkzon8Nwu78hRvfCKubJ14M5uBEwF6P */
const idx_pubkey IDX_PROGRAM_PUMP_CURVE = {{
    0x01, 0x56, 0xe0, 0xf6, 0x93, 0x66, 0x5a, 0xcf, 0x44, 0xdb, 0x15,
    0x68, 0xbf, 0x17, 0x5b, 0xaa, 0x51, 0x89, 0xcb, 0x97, 0xf5, 0xd2,
    0xff, 0x3b, 0x65, 0x5d, 0x2b, 0xb6, 0xfd, 0x6d, 0x18, 0xb0,
}};

/* pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA */
const idx_pubkey IDX_PROGRAM_PUMP_AMM = {{
    0x0c, 0x14, 0xde, 0xfc, 0x82, 0x5e, 0xc6, 0x76, 0x94, 0x25, 0x08,
    0x18, 0xbb, 0x65, 0x40, 0x65, 0xf4, 0x29, 0x8d, 0x31, 0x56, 0xd5,
    0x71, 0xb4, 0xd4, 0xf8, 0x09, 0x0c, 0x18, 0xe9, 0xa8, 0x63,
}};

/* 675kPX9MHTjS2zt1qfr1NYHuzeLXfQM9H24wFSUt1Mp8 */
const idx_pubkey IDX_PROGRAM_RAYDIUM_AMM_V4 = {{
    0x4b, 0xd9, 0x49, 0xc4, 0x36, 0x02, 0xc3, 0x3f, 0x20, 0x77, 0x90,
    0xed, 0x16, 0xa3, 0x52, 0x4c, 0xa1, 0xb9, 0x97, 0x5c, 0xf1, 0x21,
    0xa2, 0xa9, 0x0c, 0xff, 0xec, 0x7d, 0xf8, 0xb6, 0x8a, 0xcd,
}};

/* CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTaW7grrKgrWqK */
const idx_pubkey IDX_PROGRAM_RAYDIUM_CLMM = {{
    0xa5, 0xd5, 0xca, 0x9e, 0x04, 0xcf, 0x5d, 0xb5, 0x90, 0xb7, 0x14,
    0xba, 0x2f, 0xe3, 0x2c, 0xb1, 0x59, 0x13, 0x3f, 0xc1, 0xc1, 0x92,
    0xb7, 0x22, 0x57, 0xfd, 0x07, 0xd3, 0x9c, 0xb0, 0x40, 0x1e,
}};

/* CPMMoo8L3F4NbTegBCKVNunggL7H1ZpdTHKxQB5qKP1C */
const idx_pubkey IDX_PROGRAM_RAYDIUM_CPMM = {{
    0xa9, 0x2a, 0x5a, 0x8b, 0x4f, 0x29, 0x59, 0x52, 0x84, 0x25, 0x50,
    0xaa, 0x93, 0xfd, 0x5b, 0x95, 0xb5, 0xac, 0xe6, 0xa8, 0xeb, 0x92,
    0x0c, 0x93, 0x94, 0x2e, 0x43, 0x69, 0x0c, 0x20, 0xec, 0x73,
}};

/* JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4 */
const idx_pubkey IDX_PROGRAM_JUPITER = {{
    0x04, 0x79, 0xd5, 0x5b, 0xf2, 0x31, 0xc0, 0x6e, 0xee, 0x74, 0xc5,
    0x6e, 0xce, 0x68, 0x15, 0x07, 0xfd, 0xb1, 0xb2, 0xde, 0xa3, 0xf4,
    0x8e, 0x51, 0x02, 0xb1, 0xcd, 0xa2, 0x56, 0xbc, 0x13, 0x8f,
}};

/* So11111111111111111111111111111111111111112 */
const idx_pubkey IDX_MINT_WSOL = {{
    0x06, 0x9b, 0x88, 0x57, 0xfe, 0xab, 0x81, 0x84, 0xfb, 0x68, 0x7f,
    0x63, 0x46, 0x18, 0xc0, 0x35, 0xda, 0xc4, 0x39, 0xdc, 0x1a, 0xeb,
    0x3b, 0x55, 0x98, 0xa0, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x01,
}};

/* The `emit_cpi!` marker, which is sha256("anchor:event")[..8] and the same
 * for every Anchor program. */
const uint8_t IDX_ANCHOR_EVENT_DISCRIMINATOR[8] = {0xe4, 0x45, 0xa5, 0x2e,
                                                   0x51, 0xcb, 0x9a, 0x1d};

const char *idx_venue_name(idx_venue venue) {
    switch (venue) {
    case IDX_VENUE_NONE:
        return "none";
    case IDX_VENUE_PUMP_CURVE:
        return "pump_curve";
    case IDX_VENUE_PUMP_AMM:
        return "pump_amm";
    case IDX_VENUE_RAYDIUM_AMM_V4:
        return "raydium_amm_v4";
    case IDX_VENUE_RAYDIUM_CLMM:
        return "raydium_clmm";
    case IDX_VENUE_RAYDIUM_CPMM:
        return "raydium_cpmm";
    case IDX_VENUE_JUPITER:
        return "jupiter";
    }
    return "unknown";
}

idx_venue idx_venue_of_program(const idx_pubkey *program_id) {
    if (program_id == NULL) {
        return IDX_VENUE_NONE;
    }
    static const struct {
        const idx_pubkey *program;
        idx_venue venue;
    } VENUES[] = {
        {&IDX_PROGRAM_PUMP_CURVE, IDX_VENUE_PUMP_CURVE},
        {&IDX_PROGRAM_PUMP_AMM, IDX_VENUE_PUMP_AMM},
        {&IDX_PROGRAM_RAYDIUM_AMM_V4, IDX_VENUE_RAYDIUM_AMM_V4},
        {&IDX_PROGRAM_RAYDIUM_CLMM, IDX_VENUE_RAYDIUM_CLMM},
        {&IDX_PROGRAM_RAYDIUM_CPMM, IDX_VENUE_RAYDIUM_CPMM},
        {&IDX_PROGRAM_JUPITER, IDX_VENUE_JUPITER},
    };
    for (size_t i = 0; i < sizeof(VENUES) / sizeof(VENUES[0]); i++) {
        if (idx_pubkey_equal(program_id, VENUES[i].program)) {
            return VENUES[i].venue;
        }
    }
    return IDX_VENUE_NONE;
}

bool idx_venue_is_pool(idx_venue venue) {
    switch (venue) {
    case IDX_VENUE_PUMP_CURVE:
    case IDX_VENUE_PUMP_AMM:
    case IDX_VENUE_RAYDIUM_AMM_V4:
    case IDX_VENUE_RAYDIUM_CLMM:
    case IDX_VENUE_RAYDIUM_CPMM:
        return true;
    case IDX_VENUE_JUPITER:
    case IDX_VENUE_NONE:
        return false;
    }
    return false;
}

bool idx_anchor_event_payload(idx_slice data, idx_slice *payload) {
    if (data.data == NULL || data.len < sizeof(IDX_ANCHOR_EVENT_DISCRIMINATOR)) {
        return false;
    }
    if (memcmp(data.data, IDX_ANCHOR_EVENT_DISCRIMINATOR,
               sizeof(IDX_ANCHOR_EVENT_DISCRIMINATOR)) != 0) {
        return false;
    }
    if (payload != NULL) {
        *payload = idx_slice_sub(data, sizeof(IDX_ANCHOR_EVENT_DISCRIMINATOR),
                                 data.len -
                                     sizeof(IDX_ANCHOR_EVENT_DISCRIMINATOR));
    }
    return true;
}

bool idx_anchor_event_is(idx_slice payload, const uint8_t discriminator[8],
                         idx_slice *fields) {
    if (payload.data == NULL || payload.len < 8 || discriminator == NULL) {
        return false;
    }
    if (memcmp(payload.data, discriminator, 8) != 0) {
        return false;
    }
    if (fields != NULL) {
        *fields = idx_slice_sub(payload, 8, payload.len - 8);
    }
    return true;
}

idx_status idx_swap_decode(const idx_transaction *tx, const idx_instruction *ix,
                           idx_swap *out, idx_error *err) {
    if (tx == NULL || ix == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, ix and out must not be NULL");
    }
    memset(out, 0, sizeof(*out));

    idx_venue venue = idx_venue_of_program(idx_instruction_program_id(tx, ix));
    switch (venue) {
    case IDX_VENUE_PUMP_CURVE:
    case IDX_VENUE_PUMP_AMM:
        return idx_venue_pump_decode(tx, ix, venue, out, err);
    case IDX_VENUE_RAYDIUM_AMM_V4:
    case IDX_VENUE_RAYDIUM_CLMM:
    case IDX_VENUE_RAYDIUM_CPMM:
        return idx_venue_raydium_decode(tx, ix, venue, out, err);
    case IDX_VENUE_JUPITER:
        return idx_venue_jupiter_decode(tx, ix, out, err);
    case IDX_VENUE_NONE:
        break;
    }
    return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "not a venue program");
}
