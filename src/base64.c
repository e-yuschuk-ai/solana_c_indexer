#include "base64.h"

#include <string.h>

static const char k_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint8_t g_reverse[256];
static int g_reverse_ready = 0;

static void ensure_reverse_table(void) {
    if (g_reverse_ready) {
        return;
    }
    memset(g_reverse, 0xFF, sizeof(g_reverse));
    for (uint8_t i = 0; i < 64; i++) {
        g_reverse[(unsigned char)k_alphabet[i]] = i;
    }
    g_reverse_ready = 1;
}

size_t idx_base64_encoded_size(size_t byte_count) {
    return ((byte_count + 2) / 3) * 4;
}

size_t idx_base64_decoded_max(size_t text_len) { return text_len / 4 * 3; }

idx_status idx_base64_encode(idx_slice input, char *out, size_t out_size,
                             size_t *out_len, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }

    size_t needed = idx_base64_encoded_size(input.len);
    if (out_size < needed + 1) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "base64 output needs %zu bytes, got %zu", needed + 1,
                        out_size);
    }

    size_t written = 0;
    size_t i = 0;

    /* Whole three-byte groups become four characters with no padding. */
    for (; i + 3 <= input.len; i += 3) {
        uint32_t triple = ((uint32_t)input.data[i] << 16) |
                          ((uint32_t)input.data[i + 1] << 8) |
                          (uint32_t)input.data[i + 2];
        out[written++] = k_alphabet[(triple >> 18) & 0x3F];
        out[written++] = k_alphabet[(triple >> 12) & 0x3F];
        out[written++] = k_alphabet[(triple >> 6) & 0x3F];
        out[written++] = k_alphabet[triple & 0x3F];
    }

    size_t leftover = input.len - i;
    if (leftover == 1) {
        uint32_t triple = (uint32_t)input.data[i] << 16;
        out[written++] = k_alphabet[(triple >> 18) & 0x3F];
        out[written++] = k_alphabet[(triple >> 12) & 0x3F];
        out[written++] = '=';
        out[written++] = '=';
    } else if (leftover == 2) {
        uint32_t triple =
            ((uint32_t)input.data[i] << 16) | ((uint32_t)input.data[i + 1] << 8);
        out[written++] = k_alphabet[(triple >> 18) & 0x3F];
        out[written++] = k_alphabet[(triple >> 12) & 0x3F];
        out[written++] = k_alphabet[(triple >> 6) & 0x3F];
        out[written++] = '=';
    }

    out[written] = '\0';
    if (out_len != NULL) {
        *out_len = written;
    }
    return IDX_OK;
}

idx_status idx_base64_decode(const char *text, size_t text_len, uint8_t *out,
                             size_t out_size, size_t *out_len, idx_error *err) {
    if (text == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "text and out must not be NULL");
    }
    if (text_len % 4 != 0) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "base64 length %zu is not a multiple of four",
                        text_len);
    }
    ensure_reverse_table();

    if (text_len == 0) {
        if (out_len != NULL) {
            *out_len = 0;
        }
        return IDX_OK;
    }

    /* Padding only ever occupies the last one or two characters. */
    size_t padding = 0;
    if (text[text_len - 1] == '=') {
        padding++;
        if (text[text_len - 2] == '=') {
            padding++;
        }
    }

    size_t needed = text_len / 4 * 3 - padding;
    if (out_size < needed) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "base64 output needs %zu bytes, got %zu", needed,
                        out_size);
    }

    size_t written = 0;
    for (size_t i = 0; i < text_len; i += 4) {
        int is_last = (i + 4 == text_len);
        uint32_t quad = 0;

        for (size_t j = 0; j < 4; j++) {
            char c = text[i + j];
            if (c == '=') {
                /* Padding is only legal in the final group's last positions. */
                if (!is_last || j < 2 || (j == 2 && text[i + 3] != '=')) {
                    return IDX_FAIL(err, IDX_ERR_PARSE,
                                    "misplaced base64 padding at offset %zu",
                                    i + j);
                }
                quad <<= 6;
                continue;
            }
            uint8_t value = g_reverse[(unsigned char)c];
            if (value == 0xFF) {
                return IDX_FAIL(err, IDX_ERR_PARSE,
                                "invalid base64 character at offset %zu",
                                i + j);
            }
            quad = (quad << 6) | value;
        }

        size_t emit = 3;
        if (is_last) {
            emit -= padding;
        }
        if (emit > 0) {
            out[written++] = (uint8_t)((quad >> 16) & 0xFF);
        }
        if (emit > 1) {
            out[written++] = (uint8_t)((quad >> 8) & 0xFF);
        }
        if (emit > 2) {
            out[written++] = (uint8_t)(quad & 0xFF);
        }
    }

    if (out_len != NULL) {
        *out_len = written;
    }
    return IDX_OK;
}
