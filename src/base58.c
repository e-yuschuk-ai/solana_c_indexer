#include "base58.h"

#include <stdlib.h>
#include <string.h>

static const char k_alphabet[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/*
 * Reverse lookup, built once. 0xFF marks a character outside the alphabet.
 * The excluded characters are 0, O, I and l, which base58 drops because they
 * are easy to confuse when read by a human.
 */
static uint8_t g_reverse[256];
static int g_reverse_ready = 0;

static void ensure_reverse_table(void) {
    if (g_reverse_ready) {
        return;
    }
    memset(g_reverse, 0xFF, sizeof(g_reverse));
    for (uint8_t i = 0; i < 58; i++) {
        g_reverse[(unsigned char)k_alphabet[i]] = i;
    }
    g_reverse_ready = 1;
}

/*
 * Scratch space for the conversion. Keys and signatures are 32 and 64 bytes,
 * so the stack covers every call the indexer makes on its hot path; anything
 * larger falls back to the heap.
 */
#define IDX_BASE58_STACK_SCRATCH 512

typedef struct {
    uint8_t *data;
    uint8_t inlined[IDX_BASE58_STACK_SCRATCH];
    int heap;
} scratch;

static idx_status scratch_acquire(scratch *s, size_t size, idx_error *err) {
    if (size <= sizeof(s->inlined)) {
        s->data = s->inlined;
        s->heap = 0;
    } else {
        s->data = calloc(size, 1);
        if (s->data == NULL) {
            return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                            "failed to allocate %zu bytes of base58 scratch",
                            size);
        }
        s->heap = 1;
        return IDX_OK;
    }
    memset(s->data, 0, size);
    return IDX_OK;
}

static void scratch_release(scratch *s) {
    if (s->heap) {
        free(s->data);
    }
    s->data = NULL;
    s->heap = 0;
}

/* log(256)/log(58) is about 1.365, so 138/100 is a safe upper bound. */
size_t idx_base58_encoded_max(size_t byte_count) {
    return byte_count * 138 / 100 + 1;
}

/* log(58)/log(256) is about 0.733. */
size_t idx_base58_decoded_max(size_t text_len) {
    return text_len * 733 / 1000 + 1;
}

idx_status idx_base58_encode(idx_slice input, char *out, size_t out_size,
                             size_t *out_len, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }

    /* Leading zero bytes are not part of the number; each becomes a '1'. */
    size_t leading_zeros = 0;
    while (leading_zeros < input.len && input.data[leading_zeros] == 0) {
        leading_zeros++;
    }

    size_t digits_size = idx_base58_encoded_max(input.len);
    scratch digits;
    IDX_TRY(scratch_acquire(&digits, digits_size, err));

    /* Repeated division of the whole input by 58, most significant digit last. */
    size_t used = 0;
    for (size_t i = leading_zeros; i < input.len; i++) {
        uint32_t carry = input.data[i];
        size_t consumed = 0;
        for (size_t j = digits_size; j > 0 && (carry != 0 || consumed < used);
             j--, consumed++) {
            carry += 256u * digits.data[j - 1];
            digits.data[j - 1] = (uint8_t)(carry % 58);
            carry /= 58;
        }
        used = consumed;
    }

    size_t total = leading_zeros + used;
    if (out_size < total + 1) {
        scratch_release(&digits);
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "base58 output needs %zu bytes, got %zu", total + 1,
                        out_size);
    }

    memset(out, '1', leading_zeros);
    for (size_t i = 0; i < used; i++) {
        out[leading_zeros + i] = k_alphabet[digits.data[digits_size - used + i]];
    }
    out[total] = '\0';

    scratch_release(&digits);
    if (out_len != NULL) {
        *out_len = total;
    }
    return IDX_OK;
}

idx_status idx_base58_decode(const char *text, size_t text_len, uint8_t *out,
                             size_t out_size, size_t *out_len, idx_error *err) {
    if (text == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "text and out must not be NULL");
    }
    ensure_reverse_table();

    size_t leading_ones = 0;
    while (leading_ones < text_len && text[leading_ones] == '1') {
        leading_ones++;
    }

    size_t bytes_size = idx_base58_decoded_max(text_len);
    scratch bytes;
    IDX_TRY(scratch_acquire(&bytes, bytes_size, err));

    size_t used = 0;
    for (size_t i = leading_ones; i < text_len; i++) {
        uint8_t digit = g_reverse[(unsigned char)text[i]];
        if (digit == 0xFF) {
            scratch_release(&bytes);
            return IDX_FAIL(err, IDX_ERR_PARSE,
                            "invalid base58 character '%c' at offset %zu",
                            text[i], i);
        }

        uint32_t carry = digit;
        size_t consumed = 0;
        for (size_t j = bytes_size; j > 0 && (carry != 0 || consumed < used);
             j--, consumed++) {
            carry += 58u * bytes.data[j - 1];
            bytes.data[j - 1] = (uint8_t)(carry & 0xFF);
            carry >>= 8;
        }
        used = consumed;
    }

    size_t total = leading_ones + used;
    if (out_size < total) {
        scratch_release(&bytes);
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "base58 output needs %zu bytes, got %zu", total,
                        out_size);
    }

    memset(out, 0, leading_ones);
    if (used > 0) {
        memcpy(out + leading_ones, bytes.data + bytes_size - used, used);
    }

    scratch_release(&bytes);
    if (out_len != NULL) {
        *out_len = total;
    }
    return IDX_OK;
}

idx_status idx_base58_decode_exact(const char *text, size_t text_len,
                                   uint8_t *out, size_t expected_len,
                                   idx_error *err) {
    size_t decoded = 0;
    IDX_TRY(idx_base58_decode(text, text_len, out, expected_len, &decoded, err));
    if (decoded != expected_len) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "expected %zu bytes from base58, decoded %zu",
                        expected_len, decoded);
    }
    return IDX_OK;
}
