/*
 * ClickHouse row serialization (ROADMAP.md milestone M7, decision D4).
 *
 * src/ch.c sends an insert whose body is exactly the data; this is what builds
 * that body. It is the format layer and nothing else: it knows integers,
 * floats, strings and nulls, not slots, pubkeys or tables. The finalized schema
 * (the next M7 item) is what maps an entity onto a sequence of the column calls
 * below, and the batching writer after it is what decides when the accumulated
 * body is flushed.
 *
 * Two formats, written through one set of calls so a caller picks the format
 * once and never branches again:
 *
 *   RowBinary     the hot path. Compact, no escaping, no field names on the
 *                 wire — the column order alone carries the meaning.
 *   JSONEachRow   development and debugging, where readability is worth the
 *                 size. Same values, same rows, one JSON object per line.
 *
 * Both are insert formats, and deliberately so: the same write path produces
 * either, so a debugging run differs from a production one by a single enum
 * and not by a separate code path that can rot.
 *
 * RowBinary, as ClickHouse defines it and as verified against a live server:
 *
 *   - integers are fixed-width little-endian, floats IEEE 754 little-endian;
 *   - a String is a LEB128 varint byte count followed by the bytes;
 *   - a FixedString(N) is exactly N bytes with no length prefix;
 *   - a Nullable(T) is one flag byte — 1 for null, 0 for present — followed by
 *     the value *only when it is present*. A null writes the flag and nothing
 *     else.
 *
 * JSONEachRow carries one trap worth stating, because it is invisible until it
 * corrupts data. A binary column (a pubkey, a signature) is a JSON string whose
 * bytes are arbitrary, and only bytes below 0x20 may be escaped as \u00XX:
 * ClickHouse reads an escape as a *codepoint*, so an escaped 0x80 arrives as
 * the two UTF-8 bytes C2 80, and a FixedString(32) of high bytes overflows to
 * 64 and fails with TOO_LARGE_STRING_SIZE. Bytes at 0x80 and above go out
 * raw, exactly as ClickHouse's own JSON writer emits them. The consequence is
 * that a JSONEachRow body holding binary keys is not valid UTF-8 and a strict
 * JSON reader will reject it — it round-trips through ClickHouse byte for byte,
 * which is what this format is for here, but it is not a document to hand to
 * jq. The readable columns are the numbers, the enums and the nulls.
 *
 * A writer accumulates into one growable buffer and belongs to one thread, like
 * the store handles it serves (D6).
 */
#ifndef IDX_CH_ROWS_H
#define IDX_CH_ROWS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bytes.h"
#include "error.h"

typedef enum {
    IDX_CH_FORMAT_ROW_BINARY = 0,
    IDX_CH_FORMAT_JSON_EACH_ROW
} idx_ch_row_format;

/* The token a FORMAT clause takes: "RowBinary" or "JSONEachRow". Never NULL;
 * an unknown format yields "RowBinary". */
const char *idx_ch_row_format_name(idx_ch_row_format format);

typedef struct {
    idx_buffer buf;
    idx_ch_row_format format;
    size_t row_count;
    /* Bytes belonging to completed rows. The buffer may hold more while a row
     * is open, and those bytes are not part of the body. */
    size_t complete_len;
    /* Columns written into the row currently open. */
    size_t column_count;
    /* Columns the first completed row had, and therefore what every later row
     * must have; 0 until a row completes. */
    size_t row_width;
    bool in_row;
} idx_ch_rows;

/* Zero-initializes for `format`. No allocation happens until the first
 * column. */
void idx_ch_rows_init(idx_ch_rows *rows, idx_ch_row_format format);

/* Releases the buffer. Safe with NULL. */
void idx_ch_rows_free(idx_ch_rows *rows);

/*
 * Drops the accumulated rows but keeps the allocation, which is what makes the
 * batching writer's flush-and-continue cheap. An open row is abandoned; the
 * expected row width is forgotten, so a reset writer may be refilled with rows
 * of a different table.
 */
void idx_ch_rows_reset(idx_ch_rows *rows);

/* Completed rows, and the bytes they occupy. Both are what the batching
 * writer's row-count and size bounds read, and both count only completed rows —
 * a partially written one is not a unit anything may flush. */
size_t idx_ch_rows_count(const idx_ch_rows *rows);
size_t idx_ch_rows_size(const idx_ch_rows *rows);

/*
 * The accumulated body, to hand to idx_ch_insert. Empty while a row is open —
 * a half-written row is never part of the body. Invalidated by any further
 * column call.
 */
idx_slice idx_ch_rows_body(const idx_ch_rows *rows);

/* ------------------------------------------------------------------ rows -- */

/*
 * Opens a row. Columns are appended in the table's declared order, then
 * idx_ch_rows_end closes it.
 *
 *   IDX_OK               a row is open
 *   IDX_ERR_INVALID_ARG  `rows` is NULL
 *   IDX_ERR_INTERNAL     a row is already open
 */
idx_status idx_ch_rows_begin(idx_ch_rows *rows, idx_error *err);

/*
 * Closes the open row.
 *
 *   IDX_OK               the row is part of the body
 *   IDX_ERR_INVALID_ARG  `rows` is NULL
 *   IDX_ERR_INTERNAL     no row is open, or the row has a different column
 *                        count than the first one did — a schema bug caught
 *                        here rather than as a decoding error from the server
 *                        half a batch later
 *   IDX_ERR_NO_MEMORY    the row separator did not fit
 */
idx_status idx_ch_rows_end(idx_ch_rows *rows, idx_error *err);

/* --------------------------------------------------------------- columns -- */

/*
 * Every column call takes the column's `name`. RowBinary ignores it — position
 * is the only thing on the wire — and JSONEachRow uses it as the object key.
 * Passing it always is what lets one call site produce either format, and it
 * documents the column where it is written.
 *
 * Each reports IDX_ERR_INVALID_ARG when `rows` or `name` is NULL,
 * IDX_ERR_INTERNAL when no row is open, and IDX_ERR_NO_MEMORY when the buffer
 * cannot grow.
 */
idx_status idx_ch_rows_u8(idx_ch_rows *rows, const char *name, uint8_t value,
                          idx_error *err);
idx_status idx_ch_rows_u16(idx_ch_rows *rows, const char *name, uint16_t value,
                           idx_error *err);
idx_status idx_ch_rows_u32(idx_ch_rows *rows, const char *name, uint32_t value,
                           idx_error *err);
idx_status idx_ch_rows_u64(idx_ch_rows *rows, const char *name, uint64_t value,
                           idx_error *err);
idx_status idx_ch_rows_i8(idx_ch_rows *rows, const char *name, int8_t value,
                          idx_error *err);
idx_status idx_ch_rows_i16(idx_ch_rows *rows, const char *name, int16_t value,
                           idx_error *err);
idx_status idx_ch_rows_i32(idx_ch_rows *rows, const char *name, int32_t value,
                           idx_error *err);
idx_status idx_ch_rows_i64(idx_ch_rows *rows, const char *name, int64_t value,
                           idx_error *err);

/*
 * Float64. A non-finite value is written as the IEEE bits in RowBinary, and as
 * a *quoted* "nan"/"inf" in JSONEachRow — ClickHouse's JSON parser rejects them
 * bare. Neither is expected on a price, which is why it is worth not silently
 * producing a body the server refuses.
 */
idx_status idx_ch_rows_f64(idx_ch_rows *rows, const char *name, double value,
                           idx_error *err);

/* UInt8 carrying 0 or 1; JSONEachRow writes it as a number, which is what
 * ClickHouse expects for a UInt8 column. */
idx_status idx_ch_rows_bool(idx_ch_rows *rows, const char *name, bool value,
                            idx_error *err);

/* String: a varint length and the bytes. `data` may be NULL only when `len`
 * is 0. The bytes are arbitrary — see the JSONEachRow note in the file
 * comment. */
idx_status idx_ch_rows_str(idx_ch_rows *rows, const char *name,
                           const void *data, size_t len, idx_error *err);

/* FixedString(`len`): exactly `len` bytes, no prefix. The column's declared
 * width and `len` must agree; ClickHouse cannot detect a disagreement in
 * RowBinary and will silently read the following column's bytes. */
idx_status idx_ch_rows_fixed(idx_ch_rows *rows, const char *name,
                             const void *data, size_t len, idx_error *err);

/*
 * Nullable columns. `present` false writes SQL NULL and ignores the value;
 * true writes the value. This mirrors how the entity structs carry optionality
 * — a value plus a has_* flag — so a caller passes both straight through
 * rather than branching at every column.
 */
idx_status idx_ch_rows_nullable_u8(idx_ch_rows *rows, const char *name,
                                   uint8_t value, bool present, idx_error *err);
idx_status idx_ch_rows_nullable_u64(idx_ch_rows *rows, const char *name,
                                    uint64_t value, bool present,
                                    idx_error *err);
idx_status idx_ch_rows_nullable_i64(idx_ch_rows *rows, const char *name,
                                    int64_t value, bool present,
                                    idx_error *err);
idx_status idx_ch_rows_nullable_f64(idx_ch_rows *rows, const char *name,
                                    double value, bool present, idx_error *err);
idx_status idx_ch_rows_nullable_str(idx_ch_rows *rows, const char *name,
                                    const void *data, size_t len, bool present,
                                    idx_error *err);
idx_status idx_ch_rows_nullable_fixed(idx_ch_rows *rows, const char *name,
                                      const void *data, size_t len,
                                      bool present, idx_error *err);

#endif /* IDX_CH_ROWS_H */
