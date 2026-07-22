/*
 * Byte views, bounds-checked reads and a growable buffer.
 *
 * `idx_slice` is a non-owning view: it never frees and never copies, so it is
 * the type to pass around when handing decoded data to a consumer.
 * `idx_cursor` walks a slice with bounds checks, which is how binary decoding
 * in M5 avoids open-coded pointer arithmetic.
 * `idx_buffer` owns heap memory and grows; it backs things that outlive a
 * single unit of work, such as WebSocket frame reassembly.
 */
#ifndef IDX_BYTES_H
#define IDX_BYTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"

/* ---------------------------------------------------------------- slice -- */

typedef struct {
    const uint8_t *data;
    size_t len;
} idx_slice;

idx_slice idx_slice_make(const void *data, size_t len);

/* View over a NUL-terminated string, excluding the terminator. */
idx_slice idx_slice_from_str(const char *str);

bool idx_slice_is_empty(idx_slice slice);
bool idx_slice_equal(idx_slice a, idx_slice b);

/*
 * Sub-slice starting at `offset` and at most `len` bytes long, clamped to the
 * end of the input. Out-of-range offsets yield an empty slice.
 */
idx_slice idx_slice_sub(idx_slice slice, size_t offset, size_t len);

/* --------------------------------------------------------------- cursor -- */

typedef struct {
    idx_slice input;
    size_t offset;
} idx_cursor;

void idx_cursor_init(idx_cursor *cursor, idx_slice input);
size_t idx_cursor_remaining(const idx_cursor *cursor);
bool idx_cursor_exhausted(const idx_cursor *cursor);

/* Borrows `len` bytes without copying and advances past them. */
idx_status idx_cursor_take(idx_cursor *cursor, size_t len, idx_slice *out,
                           idx_error *err);

/* Copies `len` bytes into `dest` and advances past them. */
idx_status idx_cursor_copy(idx_cursor *cursor, void *dest, size_t len,
                           idx_error *err);

idx_status idx_cursor_skip(idx_cursor *cursor, size_t len, idx_error *err);

/* Little-endian, matching the encoding Solana uses for on-chain data. */
idx_status idx_cursor_u8(idx_cursor *cursor, uint8_t *out, idx_error *err);
idx_status idx_cursor_u16le(idx_cursor *cursor, uint16_t *out, idx_error *err);
idx_status idx_cursor_u32le(idx_cursor *cursor, uint32_t *out, idx_error *err);
idx_status idx_cursor_u64le(idx_cursor *cursor, uint64_t *out, idx_error *err);

/* --------------------------------------------------------------- buffer -- */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t capacity;
} idx_buffer;

/* Zero-initializes. No allocation happens until the first append. */
void idx_buffer_init(idx_buffer *buffer);

/* Ensures room for `additional` more bytes beyond the current length. */
idx_status idx_buffer_reserve(idx_buffer *buffer, size_t additional,
                              idx_error *err);

idx_status idx_buffer_append(idx_buffer *buffer, const void *data, size_t len,
                             idx_error *err);
idx_status idx_buffer_append_byte(idx_buffer *buffer, uint8_t byte,
                                  idx_error *err);

/* Drops the contents but keeps the allocation for reuse. */
void idx_buffer_clear(idx_buffer *buffer);

void idx_buffer_free(idx_buffer *buffer);

/* View over the buffer's contents. Invalidated by any further append. */
idx_slice idx_buffer_slice(const idx_buffer *buffer);

/* ------------------------------------------------------------------ hex -- */

/* Characters needed to encode `byte_count` bytes, excluding the terminator. */
size_t idx_hex_encoded_size(size_t byte_count);

/* Writes lowercase hex plus a NUL terminator. */
idx_status idx_hex_encode(idx_slice input, char *out, size_t out_size,
                          idx_error *err);

/* Accepts either case. `text_len` must be even. */
idx_status idx_hex_decode(const char *text, size_t text_len, uint8_t *out,
                          size_t out_size, size_t *out_len, idx_error *err);

#endif /* IDX_BYTES_H */
