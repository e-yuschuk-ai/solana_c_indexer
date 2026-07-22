/*
 * Base64 as defined by RFC 4648, with padding.
 *
 * Solana returns account data and, depending on the requested encoding,
 * transactions in this form.
 */
#ifndef IDX_BASE64_H
#define IDX_BASE64_H

#include <stddef.h>
#include <stdint.h>

#include "bytes.h"
#include "error.h"

/* Encoded characters for `byte_count` bytes, excluding the NUL terminator. */
size_t idx_base64_encoded_size(size_t byte_count);

/* Upper bound on decoded bytes for `text_len` characters. */
size_t idx_base64_decoded_max(size_t text_len);

/* Writes the encoding plus a NUL terminator. */
idx_status idx_base64_encode(idx_slice input, char *out, size_t out_size,
                             size_t *out_len, idx_error *err);

/*
 * Decodes `text_len` characters. Padding is required and the length must be a
 * multiple of four; whitespace and any character outside the alphabet are
 * rejected rather than skipped, because the inputs come from an RPC response
 * and silently tolerating malformed data hides bugs.
 */
idx_status idx_base64_decode(const char *text, size_t text_len, uint8_t *out,
                             size_t out_size, size_t *out_len, idx_error *err);

#endif /* IDX_BASE64_H */
