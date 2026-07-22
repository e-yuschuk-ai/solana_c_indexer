#include "bytes.h"

#include <stdlib.h>
#include <string.h>

#define IDX_BUFFER_MIN_CAPACITY 64

/* ---------------------------------------------------------------- slice -- */

idx_slice idx_slice_make(const void *data, size_t len) {
    idx_slice slice;
    slice.data = (const uint8_t *)data;
    slice.len = (data != NULL) ? len : 0;
    return slice;
}

idx_slice idx_slice_from_str(const char *str) {
    return idx_slice_make(str, (str != NULL) ? strlen(str) : 0);
}

bool idx_slice_is_empty(idx_slice slice) { return slice.len == 0; }

bool idx_slice_equal(idx_slice a, idx_slice b) {
    if (a.len != b.len) {
        return false;
    }
    if (a.len == 0) {
        return true;
    }
    return memcmp(a.data, b.data, a.len) == 0;
}

idx_slice idx_slice_sub(idx_slice slice, size_t offset, size_t len) {
    if (offset >= slice.len) {
        return idx_slice_make(NULL, 0);
    }
    size_t available = slice.len - offset;
    return idx_slice_make(slice.data + offset,
                          (len < available) ? len : available);
}

/* --------------------------------------------------------------- cursor -- */

void idx_cursor_init(idx_cursor *cursor, idx_slice input) {
    cursor->input = input;
    cursor->offset = 0;
}

size_t idx_cursor_remaining(const idx_cursor *cursor) {
    return cursor->input.len - cursor->offset;
}

bool idx_cursor_exhausted(const idx_cursor *cursor) {
    return idx_cursor_remaining(cursor) == 0;
}

idx_status idx_cursor_take(idx_cursor *cursor, size_t len, idx_slice *out,
                           idx_error *err) {
    if (cursor == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "cursor and out must not be NULL");
    }
    if (len > idx_cursor_remaining(cursor)) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "need %zu bytes at offset %zu, only %zu remain", len,
                        cursor->offset, idx_cursor_remaining(cursor));
    }
    *out = idx_slice_make(cursor->input.data + cursor->offset, len);
    cursor->offset += len;
    return IDX_OK;
}

idx_status idx_cursor_copy(idx_cursor *cursor, void *dest, size_t len,
                           idx_error *err) {
    /* Initialized because the optimizer cannot see that a successful take
     * always writes it. */
    idx_slice taken = idx_slice_make(NULL, 0);
    IDX_TRY(idx_cursor_take(cursor, len, &taken, err));
    if (len > 0) {
        memcpy(dest, taken.data, len);
    }
    return IDX_OK;
}

idx_status idx_cursor_skip(idx_cursor *cursor, size_t len, idx_error *err) {
    idx_slice ignored = idx_slice_make(NULL, 0);
    return idx_cursor_take(cursor, len, &ignored, err);
}

idx_status idx_cursor_u8(idx_cursor *cursor, uint8_t *out, idx_error *err) {
    return idx_cursor_copy(cursor, out, 1, err);
}

idx_status idx_cursor_u16le(idx_cursor *cursor, uint16_t *out, idx_error *err) {
    uint8_t raw[2];
    IDX_TRY(idx_cursor_copy(cursor, raw, sizeof(raw), err));
    *out = (uint16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    return IDX_OK;
}

idx_status idx_cursor_u32le(idx_cursor *cursor, uint32_t *out, idx_error *err) {
    uint8_t raw[4];
    IDX_TRY(idx_cursor_copy(cursor, raw, sizeof(raw), err));
    *out = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) |
           ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
    return IDX_OK;
}

idx_status idx_cursor_u64le(idx_cursor *cursor, uint64_t *out, idx_error *err) {
    uint8_t raw[8];
    IDX_TRY(idx_cursor_copy(cursor, raw, sizeof(raw), err));
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(raw); i++) {
        value |= (uint64_t)raw[i] << (8 * i);
    }
    *out = value;
    return IDX_OK;
}

/* --------------------------------------------------------------- buffer -- */

void idx_buffer_init(idx_buffer *buffer) {
    if (buffer == NULL) {
        return;
    }
    buffer->data = NULL;
    buffer->len = 0;
    buffer->capacity = 0;
}

idx_status idx_buffer_reserve(idx_buffer *buffer, size_t additional,
                              idx_error *err) {
    if (buffer == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "buffer must not be NULL");
    }
    if (additional <= buffer->capacity - buffer->len) {
        return IDX_OK;
    }
    if (additional > SIZE_MAX - buffer->len) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "buffer growth of %zu bytes overflows", additional);
    }

    size_t needed = buffer->len + additional;
    size_t capacity = (buffer->capacity > 0) ? buffer->capacity
                                             : IDX_BUFFER_MIN_CAPACITY;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }

    uint8_t *grown = realloc(buffer->data, capacity);
    if (grown == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to grow buffer to %zu bytes", capacity);
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return IDX_OK;
}

idx_status idx_buffer_append(idx_buffer *buffer, const void *data, size_t len,
                             idx_error *err) {
    if (len == 0) {
        return IDX_OK;
    }
    if (data == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "data must not be NULL when len is %zu", len);
    }
    IDX_TRY(idx_buffer_reserve(buffer, len, err));
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    return IDX_OK;
}

idx_status idx_buffer_append_byte(idx_buffer *buffer, uint8_t byte,
                                  idx_error *err) {
    return idx_buffer_append(buffer, &byte, 1, err);
}

void idx_buffer_clear(idx_buffer *buffer) {
    if (buffer != NULL) {
        buffer->len = 0;
    }
}

void idx_buffer_free(idx_buffer *buffer) {
    if (buffer == NULL) {
        return;
    }
    free(buffer->data);
    idx_buffer_init(buffer);
}

idx_slice idx_buffer_slice(const idx_buffer *buffer) {
    if (buffer == NULL) {
        return idx_slice_make(NULL, 0);
    }
    return idx_slice_make(buffer->data, buffer->len);
}

/* ------------------------------------------------------------------ hex -- */

static const char k_hex_digits[] = "0123456789abcdef";

size_t idx_hex_encoded_size(size_t byte_count) { return byte_count * 2; }

idx_status idx_hex_encode(idx_slice input, char *out, size_t out_size,
                          idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    size_t needed = idx_hex_encoded_size(input.len) + 1;
    if (out_size < needed) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "hex output needs %zu bytes, got %zu", needed,
                        out_size);
    }
    for (size_t i = 0; i < input.len; i++) {
        out[i * 2] = k_hex_digits[input.data[i] >> 4];
        out[i * 2 + 1] = k_hex_digits[input.data[i] & 0x0F];
    }
    out[input.len * 2] = '\0';
    return IDX_OK;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

idx_status idx_hex_decode(const char *text, size_t text_len, uint8_t *out,
                          size_t out_size, size_t *out_len, idx_error *err) {
    if (text == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "text and out must not be NULL");
    }
    if (text_len % 2 != 0) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "hex input has an odd length of %zu", text_len);
    }

    size_t needed = text_len / 2;
    if (out_size < needed) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "hex output needs %zu bytes, got %zu", needed,
                        out_size);
    }

    for (size_t i = 0; i < needed; i++) {
        int high = hex_value(text[i * 2]);
        int low = hex_value(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return IDX_FAIL(err, IDX_ERR_PARSE,
                            "invalid hex digit at offset %zu", i * 2);
        }
        out[i] = (uint8_t)((high << 4) | low);
    }

    if (out_len != NULL) {
        *out_len = needed;
    }
    return IDX_OK;
}
