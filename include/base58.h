/*
 * Base58 with the Bitcoin alphabet, which is what Solana uses for pubkeys,
 * signatures and blockhashes.
 *
 * Encoding is a repeated big-number division, so cost is quadratic in the
 * input length. That is irrelevant for the 32 and 64 byte values this project
 * encodes, but it is the reason the API is not offered for arbitrary payloads.
 */
#ifndef IDX_BASE58_H
#define IDX_BASE58_H

#include <stddef.h>
#include <stdint.h>

#include "bytes.h"
#include "error.h"

/* Upper bound on encoded characters, excluding the NUL terminator. */
size_t idx_base58_encoded_max(size_t byte_count);

/* Upper bound on decoded bytes for `text_len` characters. */
size_t idx_base58_decoded_max(size_t text_len);

/*
 * Writes the encoding plus a NUL terminator. `out_len`, when not NULL,
 * receives the length excluding the terminator.
 */
idx_status idx_base58_encode(idx_slice input, char *out, size_t out_size,
                             size_t *out_len, idx_error *err);

/*
 * Decodes `text_len` characters. Rejects any character outside the alphabet,
 * including whitespace.
 */
idx_status idx_base58_decode(const char *text, size_t text_len, uint8_t *out,
                             size_t out_size, size_t *out_len, idx_error *err);

/* Decodes expecting exactly `expected_len` bytes; the common case for keys. */
idx_status idx_base58_decode_exact(const char *text, size_t text_len,
                                   uint8_t *out, size_t expected_len,
                                   idx_error *err);

#endif /* IDX_BASE58_H */
