#include "types.h"

#include <string.h>

#include "base58.h"

const idx_pubkey IDX_PUBKEY_DEFAULT = {{0}};

/*
 * Program ids, as raw bytes rather than base58 text, so they need no decoding
 * at startup and can be compared directly. The base58 form of each is in the
 * comment; tests assert the two agree.
 */

/* 11111111111111111111111111111111 */
const idx_pubkey IDX_PROGRAM_SYSTEM = {{0}};

/* TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA */
const idx_pubkey IDX_PROGRAM_TOKEN = {{
    0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93, 0xd9, 0xcb, 0xe1,
    0x46, 0xce, 0xeb, 0x79, 0xac, 0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b,
    0x37, 0x91, 0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9,
}};

/* TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb */
const idx_pubkey IDX_PROGRAM_TOKEN_2022 = {{
    0x06, 0xdd, 0xf6, 0xe1, 0xee, 0x75, 0x8f, 0xde, 0x18, 0x42, 0x5d,
    0xbc, 0xe4, 0x6c, 0xcd, 0xda, 0xb6, 0x1a, 0xfc, 0x4d, 0x83, 0xb9,
    0x0d, 0x27, 0xfe, 0xbd, 0xf9, 0x28, 0xd8, 0xa1, 0x8b, 0xfc,
}};

/* MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr */
const idx_pubkey IDX_PROGRAM_MEMO = {{
    0x05, 0x4a, 0x53, 0x5a, 0x99, 0x29, 0x21, 0x06, 0x4d, 0x24, 0xe8,
    0x71, 0x60, 0xda, 0x38, 0x7c, 0x7c, 0x35, 0xb5, 0xdd, 0xbc, 0x92,
    0xbb, 0x81, 0xe4, 0x1f, 0xa8, 0x40, 0x41, 0x05, 0x44, 0x8d,
}};

/* ---------------------------------------------------------------- pubkey -- */

idx_pubkey idx_pubkey_from_bytes(const uint8_t bytes[IDX_PUBKEY_LEN]) {
    idx_pubkey key;
    memcpy(key.bytes, bytes, IDX_PUBKEY_LEN);
    return key;
}

bool idx_pubkey_equal(const idx_pubkey *a, const idx_pubkey *b) {
    return memcmp(a->bytes, b->bytes, IDX_PUBKEY_LEN) == 0;
}

bool idx_pubkey_is_default(const idx_pubkey *key) {
    return idx_pubkey_equal(key, &IDX_PUBKEY_DEFAULT);
}

idx_slice idx_pubkey_slice(const idx_pubkey *key) {
    return idx_slice_make(key->bytes, IDX_PUBKEY_LEN);
}

idx_status idx_pubkey_from_base58(const char *text, size_t text_len,
                                  idx_pubkey *out, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    return idx_base58_decode_exact(text, text_len, out->bytes, IDX_PUBKEY_LEN,
                                   err);
}

idx_status idx_pubkey_to_base58(const idx_pubkey *key,
                                char out[IDX_PUBKEY_STR_MAX], idx_error *err) {
    if (key == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "key and out must not be NULL");
    }
    return idx_base58_encode(idx_pubkey_slice(key), out, IDX_PUBKEY_STR_MAX,
                             NULL, err);
}

/* ------------------------------------------------------------- signature -- */

idx_status idx_signature_from_base58(const char *text, size_t text_len,
                                     idx_signature *out, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    return idx_base58_decode_exact(text, text_len, out->bytes,
                                   IDX_SIGNATURE_LEN, err);
}

idx_status idx_signature_to_base58(const idx_signature *signature,
                                   char out[IDX_SIGNATURE_STR_MAX],
                                   idx_error *err) {
    if (signature == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "signature and out must not be NULL");
    }
    return idx_base58_encode(
        idx_slice_make(signature->bytes, IDX_SIGNATURE_LEN), out,
        IDX_SIGNATURE_STR_MAX, NULL, err);
}

bool idx_signature_equal(const idx_signature *a, const idx_signature *b) {
    return memcmp(a->bytes, b->bytes, IDX_SIGNATURE_LEN) == 0;
}

/* ------------------------------------------------------------------ hash -- */

idx_status idx_hash_from_base58(const char *text, size_t text_len,
                                idx_hash *out, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    return idx_base58_decode_exact(text, text_len, out->bytes, IDX_HASH_LEN,
                                   err);
}

idx_status idx_hash_to_base58(const idx_hash *hash, char out[IDX_HASH_STR_MAX],
                              idx_error *err) {
    if (hash == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "hash and out must not be NULL");
    }
    return idx_base58_encode(idx_slice_make(hash->bytes, IDX_HASH_LEN), out,
                             IDX_HASH_STR_MAX, NULL, err);
}

bool idx_hash_equal(const idx_hash *a, const idx_hash *b) {
    return memcmp(a->bytes, b->bytes, IDX_HASH_LEN) == 0;
}
