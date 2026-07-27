/*
 * ClickHouse row serialization. See include/ch_rows.h for the format rules and
 * why both formats are insert formats.
 *
 * Every column call routes through one of the two writers below: a binary one
 * that appends fixed-width little-endian bytes, and a JSON one that appends
 * text. The shared entry points (column_begin, column_end) do the argument and
 * state checks once so each column function stays a two-line dispatch.
 */
#include "ch_rows.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* A double in %.17g, or "nan"/"-inf", plus quotes and a terminator. */
#define DBL_BUF 32
/* A uint64 in decimal, with room to spare. */
#define NUM_BUF 24

const char *idx_ch_row_format_name(idx_ch_row_format format) {
    return format == IDX_CH_FORMAT_JSON_EACH_ROW ? "JSONEachRow" : "RowBinary";
}

void idx_ch_rows_init(idx_ch_rows *rows, idx_ch_row_format format) {
    if (rows == NULL) {
        return;
    }
    idx_buffer_init(&rows->buf);
    rows->format = format;
    rows->row_count = 0;
    rows->complete_len = 0;
    rows->column_count = 0;
    rows->row_width = 0;
    rows->in_row = false;
}

void idx_ch_rows_free(idx_ch_rows *rows) {
    if (rows == NULL) {
        return;
    }
    idx_buffer_free(&rows->buf);
    rows->row_count = 0;
    rows->complete_len = 0;
    rows->column_count = 0;
    rows->row_width = 0;
    rows->in_row = false;
}

void idx_ch_rows_reset(idx_ch_rows *rows) {
    if (rows == NULL) {
        return;
    }
    idx_buffer_clear(&rows->buf);
    rows->row_count = 0;
    rows->complete_len = 0;
    rows->column_count = 0;
    rows->row_width = 0;
    rows->in_row = false;
}

size_t idx_ch_rows_count(const idx_ch_rows *rows) {
    return rows != NULL ? rows->row_count : 0;
}

size_t idx_ch_rows_size(const idx_ch_rows *rows) {
    return rows != NULL ? rows->complete_len : 0;
}

idx_slice idx_ch_rows_body(const idx_ch_rows *rows) {
    if (rows == NULL) {
        return idx_slice_make(NULL, 0);
    }
    return idx_slice_make(rows->buf.data, rows->complete_len);
}

/* ------------------------------------------------------------- row state -- */

idx_status idx_ch_rows_begin(idx_ch_rows *rows, idx_error *err) {
    if (rows == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "ch rows begin: null writer");
    }
    if (rows->in_row) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL,
                        "ch rows begin: row already open");
    }
    rows->in_row = true;
    rows->column_count = 0;
    if (rows->format == IDX_CH_FORMAT_JSON_EACH_ROW) {
        IDX_TRY(idx_buffer_append_byte(&rows->buf, '{', err));
    }
    return IDX_OK;
}

idx_status idx_ch_rows_end(idx_ch_rows *rows, idx_error *err) {
    if (rows == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "ch rows end: null writer");
    }
    if (!rows->in_row) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL, "ch rows end: no row open");
    }
    /* A row narrower or wider than the first one is a schema bug. RowBinary
     * cannot express it — the server would read the next row's bytes as this
     * row's tail and report a decoding error somewhere further along — so it is
     * worth refusing here, where the column that is missing is still on the
     * stack. */
    if (rows->row_width != 0 && rows->column_count != rows->row_width) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL,
                        "ch rows end: row %zu has %zu columns, expected %zu",
                        rows->row_count, rows->column_count, rows->row_width);
    }
    if (rows->format == IDX_CH_FORMAT_JSON_EACH_ROW) {
        IDX_TRY(idx_buffer_append(&rows->buf, "}\n", 2, err));
    }
    rows->complete_len = rows->buf.len;
    rows->row_width = rows->column_count;
    rows->row_count++;
    rows->in_row = false;
    rows->column_count = 0;
    return IDX_OK;
}

/*
 * Shared prologue for every column: validates, and in JSON writes the
 * separator and the key so each column function only has to write its value.
 */
static idx_status column_begin(idx_ch_rows *rows, const char *name,
                               idx_error *err) {
    if (rows == NULL || name == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "ch rows column: null %s",
                        rows == NULL ? "writer" : "name");
    }
    if (!rows->in_row) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL, "ch rows column %s: no row open",
                        name);
    }
    if (rows->format == IDX_CH_FORMAT_JSON_EACH_ROW) {
        if (rows->column_count > 0) {
            IDX_TRY(idx_buffer_append_byte(&rows->buf, ',', err));
        }
        IDX_TRY(idx_buffer_append_byte(&rows->buf, '"', err));
        IDX_TRY(idx_buffer_append(&rows->buf, name, strlen(name), err));
        IDX_TRY(idx_buffer_append(&rows->buf, "\":", 2, err));
    }
    return IDX_OK;
}

/* Counts the column. Only called once its value is fully written, so a failed
 * column never advances the count. */
static idx_status column_end(idx_ch_rows *rows) {
    rows->column_count++;
    return IDX_OK;
}

/* ------------------------------------------------------------ primitives -- */

/* `width` bytes of `value`, little-endian. Covers every signed and unsigned
 * integer: two's complement means the signed ones need no separate path. */
static idx_status put_le(idx_ch_rows *rows, uint64_t value, size_t width,
                         idx_error *err) {
    uint8_t raw[8];
    for (size_t i = 0; i < width; i++) {
        raw[i] = (uint8_t)(value >> (8 * i));
    }
    return idx_buffer_append(&rows->buf, raw, width, err);
}

/* LEB128, ClickHouse's length prefix for a String. */
static idx_status put_varint(idx_ch_rows *rows, uint64_t value,
                             idx_error *err) {
    uint8_t raw[10];
    size_t n = 0;
    do {
        uint8_t byte = (uint8_t)(value & 0x7f);
        value >>= 7;
        if (value != 0) {
            byte = (uint8_t)(byte | 0x80);
        }
        raw[n++] = byte;
    } while (value != 0);
    return idx_buffer_append(&rows->buf, raw, n, err);
}

static idx_status put_text(idx_ch_rows *rows, const char *text,
                           idx_error *err) {
    return idx_buffer_append(&rows->buf, text, strlen(text), err);
}

/*
 * A JSON string holding arbitrary bytes. Escapes exactly what JSON requires and
 * nothing more: a quote, a backslash, and the controls below 0x20. Bytes at
 * 0x80 and above are written raw — escaping them would expand each into two
 * UTF-8 bytes on the server and overflow a FixedString (see ch_rows.h).
 */
static idx_status put_json_bytes(idx_ch_rows *rows, const uint8_t *data,
                                 size_t len, idx_error *err) {
    IDX_TRY(idx_buffer_append_byte(&rows->buf, '"', err));
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        switch (byte) {
        case '"':
            IDX_TRY(idx_buffer_append(&rows->buf, "\\\"", 2, err));
            continue;
        case '\\':
            IDX_TRY(idx_buffer_append(&rows->buf, "\\\\", 2, err));
            continue;
        case '\b':
            IDX_TRY(idx_buffer_append(&rows->buf, "\\b", 2, err));
            continue;
        case '\f':
            IDX_TRY(idx_buffer_append(&rows->buf, "\\f", 2, err));
            continue;
        case '\n':
            IDX_TRY(idx_buffer_append(&rows->buf, "\\n", 2, err));
            continue;
        case '\r':
            IDX_TRY(idx_buffer_append(&rows->buf, "\\r", 2, err));
            continue;
        case '\t':
            IDX_TRY(idx_buffer_append(&rows->buf, "\\t", 2, err));
            continue;
        default:
            break;
        }
        if (byte < 0x20) {
            char esc[7];
            snprintf(esc, sizeof esc, "\\u%04X", byte);
            IDX_TRY(put_text(rows, esc, err));
            continue;
        }
        IDX_TRY(idx_buffer_append_byte(&rows->buf, byte, err));
    }
    return idx_buffer_append_byte(&rows->buf, '"', err);
}

/* The JSON null literal, for a Nullable column with nothing in it. */
static idx_status put_json_null(idx_ch_rows *rows, idx_error *err) {
    return idx_buffer_append(&rows->buf, "null", 4, err);
}

/*
 * The Nullable(T) flag byte in RowBinary: 1 for null, 0 for present. The value
 * follows only when present, which is why this is a separate step rather than
 * something the value writers fold in.
 */
static idx_status put_null_flag(idx_ch_rows *rows, bool present,
                                idx_error *err) {
    return idx_buffer_append_byte(&rows->buf, present ? (uint8_t)0 : (uint8_t)1,
                                  err);
}

/* ------------------------------------------------------- integer columns -- */

/* The whole unsigned family: `width` bytes binary, or decimal text in JSON. */
static idx_status put_uint(idx_ch_rows *rows, uint64_t value, size_t width,
                           idx_error *err) {
    if (rows->format == IDX_CH_FORMAT_ROW_BINARY) {
        return put_le(rows, value, width, err);
    }
    char text[NUM_BUF];
    snprintf(text, sizeof text, "%" PRIu64, value);
    return put_text(rows, text, err);
}

static idx_status put_int(idx_ch_rows *rows, int64_t value, size_t width,
                          idx_error *err) {
    if (rows->format == IDX_CH_FORMAT_ROW_BINARY) {
        return put_le(rows, (uint64_t)value, width, err);
    }
    char text[NUM_BUF];
    snprintf(text, sizeof text, "%" PRId64, value);
    return put_text(rows, text, err);
}

#define DEFINE_UINT_COLUMN(suffix, type, width)                              \
    idx_status idx_ch_rows_##suffix(idx_ch_rows *rows, const char *name,     \
                                    type value, idx_error *err) {            \
        IDX_TRY(column_begin(rows, name, err));                              \
        IDX_TRY(put_uint(rows, (uint64_t)value, (width), err));              \
        return column_end(rows);                                             \
    }

#define DEFINE_INT_COLUMN(suffix, type, width)                               \
    idx_status idx_ch_rows_##suffix(idx_ch_rows *rows, const char *name,     \
                                    type value, idx_error *err) {            \
        IDX_TRY(column_begin(rows, name, err));                              \
        IDX_TRY(put_int(rows, (int64_t)value, (width), err));                \
        return column_end(rows);                                             \
    }

DEFINE_UINT_COLUMN(u8, uint8_t, 1)
DEFINE_UINT_COLUMN(u16, uint16_t, 2)
DEFINE_UINT_COLUMN(u32, uint32_t, 4)
DEFINE_UINT_COLUMN(u64, uint64_t, 8)
DEFINE_INT_COLUMN(i8, int8_t, 1)
DEFINE_INT_COLUMN(i16, int16_t, 2)
DEFINE_INT_COLUMN(i32, int32_t, 4)
DEFINE_INT_COLUMN(i64, int64_t, 8)

#undef DEFINE_UINT_COLUMN
#undef DEFINE_INT_COLUMN

idx_status idx_ch_rows_bool(idx_ch_rows *rows, const char *name, bool value,
                            idx_error *err) {
    return idx_ch_rows_u8(rows, name, value ? 1 : 0, err);
}

/* --------------------------------------------------------- float columns -- */

/*
 * Float64. The binary side is the IEEE bits; memcpy rather than a cast through
 * a pointer, which would be a strict-aliasing violation.
 *
 * The JSON side prints %.17g, the shortest form that round-trips a double, and
 * quotes a non-finite one: ClickHouse's JSON parser accepts "nan" and "inf" as
 * strings and rejects them bare (verified against 24.8).
 */
static idx_status put_double(idx_ch_rows *rows, double value, idx_error *err) {
    if (rows->format == IDX_CH_FORMAT_ROW_BINARY) {
        uint64_t bits;
        memcpy(&bits, &value, sizeof bits);
        return put_le(rows, bits, 8, err);
    }
    char text[DBL_BUF];
    if (isfinite(value)) {
        snprintf(text, sizeof text, "%.17g", value);
    } else {
        snprintf(text, sizeof text, "\"%.17g\"", value);
    }
    return put_text(rows, text, err);
}

idx_status idx_ch_rows_f64(idx_ch_rows *rows, const char *name, double value,
                           idx_error *err) {
    IDX_TRY(column_begin(rows, name, err));
    IDX_TRY(put_double(rows, value, err));
    return column_end(rows);
}

/* -------------------------------------------------------- string columns -- */

static idx_status put_string(idx_ch_rows *rows, const void *data, size_t len,
                             idx_error *err) {
    const uint8_t *bytes = (const uint8_t *)data;
    if (rows->format == IDX_CH_FORMAT_ROW_BINARY) {
        IDX_TRY(put_varint(rows, (uint64_t)len, err));
        return idx_buffer_append(&rows->buf, bytes, len, err);
    }
    return put_json_bytes(rows, bytes, len, err);
}

/* FixedString: the same bytes without the varint, since the column's width is
 * declared rather than carried. */
static idx_status put_fixed(idx_ch_rows *rows, const void *data, size_t len,
                            idx_error *err) {
    const uint8_t *bytes = (const uint8_t *)data;
    if (rows->format == IDX_CH_FORMAT_ROW_BINARY) {
        return idx_buffer_append(&rows->buf, bytes, len, err);
    }
    return put_json_bytes(rows, bytes, len, err);
}

/* Both string shapes accept a NULL pointer only for an empty value. */
static idx_status check_bytes(const void *data, size_t len, const char *name,
                              idx_error *err) {
    if (data == NULL && len > 0) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "ch rows column %s: null data with length %zu", name,
                        len);
    }
    return IDX_OK;
}

/* check_bytes runs before column_begin throughout: a rejected argument must not
 * leave a half-written column in the buffer (docs/conventions.md, "validate
 * first, then mutate"). */
idx_status idx_ch_rows_str(idx_ch_rows *rows, const char *name,
                           const void *data, size_t len, idx_error *err) {
    IDX_TRY(check_bytes(data, len, name != NULL ? name : "?", err));
    IDX_TRY(column_begin(rows, name, err));
    IDX_TRY(put_string(rows, data, len, err));
    return column_end(rows);
}

idx_status idx_ch_rows_fixed(idx_ch_rows *rows, const char *name,
                             const void *data, size_t len, idx_error *err) {
    IDX_TRY(check_bytes(data, len, name != NULL ? name : "?", err));
    IDX_TRY(column_begin(rows, name, err));
    IDX_TRY(put_fixed(rows, data, len, err));
    return column_end(rows);
}

/* ------------------------------------------------------ nullable columns -- */

/*
 * Each of these is the same shape: the null marker, then the value only when
 * present. In RowBinary the marker is a flag byte and an absent value writes no
 * further bytes; in JSON an absent value is the literal null and the marker is
 * the value itself.
 */
#define NULLABLE_PROLOGUE(rows, name, present, err)                          \
    do {                                                                     \
        IDX_TRY(column_begin((rows), (name), (err)));                        \
        if ((rows)->format == IDX_CH_FORMAT_ROW_BINARY) {                    \
            IDX_TRY(put_null_flag((rows), (present), (err)));                \
        }                                                                    \
        if (!(present)) {                                                    \
            if ((rows)->format == IDX_CH_FORMAT_JSON_EACH_ROW) {             \
                IDX_TRY(put_json_null((rows), (err)));                       \
            }                                                                \
            return column_end(rows);                                         \
        }                                                                    \
    } while (0)

idx_status idx_ch_rows_nullable_u8(idx_ch_rows *rows, const char *name,
                                   uint8_t value, bool present,
                                   idx_error *err) {
    NULLABLE_PROLOGUE(rows, name, present, err);
    IDX_TRY(put_uint(rows, value, 1, err));
    return column_end(rows);
}

idx_status idx_ch_rows_nullable_u64(idx_ch_rows *rows, const char *name,
                                    uint64_t value, bool present,
                                    idx_error *err) {
    NULLABLE_PROLOGUE(rows, name, present, err);
    IDX_TRY(put_uint(rows, value, 8, err));
    return column_end(rows);
}

idx_status idx_ch_rows_nullable_i64(idx_ch_rows *rows, const char *name,
                                    int64_t value, bool present,
                                    idx_error *err) {
    NULLABLE_PROLOGUE(rows, name, present, err);
    IDX_TRY(put_int(rows, value, 8, err));
    return column_end(rows);
}

idx_status idx_ch_rows_nullable_f64(idx_ch_rows *rows, const char *name,
                                    double value, bool present,
                                    idx_error *err) {
    NULLABLE_PROLOGUE(rows, name, present, err);
    IDX_TRY(put_double(rows, value, err));
    return column_end(rows);
}

idx_status idx_ch_rows_nullable_str(idx_ch_rows *rows, const char *name,
                                    const void *data, size_t len, bool present,
                                    idx_error *err) {
    if (present) {
        IDX_TRY(check_bytes(data, len, name != NULL ? name : "?", err));
    }
    NULLABLE_PROLOGUE(rows, name, present, err);
    IDX_TRY(put_string(rows, data, len, err));
    return column_end(rows);
}

idx_status idx_ch_rows_nullable_fixed(idx_ch_rows *rows, const char *name,
                                      const void *data, size_t len,
                                      bool present, idx_error *err) {
    if (present) {
        IDX_TRY(check_bytes(data, len, name != NULL ? name : "?", err));
    }
    NULLABLE_PROLOGUE(rows, name, present, err);
    IDX_TRY(put_fixed(rows, data, len, err));
    return column_end(rows);
}

#undef NULLABLE_PROLOGUE
