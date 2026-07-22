/*
 * JSON reading and minimal writing.
 *
 * This is the only interface the rest of the project uses; yyjson appears
 * nowhere outside src/json.c, so the parser can be replaced without touching
 * call sites (decision D2).
 *
 * Reading is designed for the shape of an RPC response: reach into an object
 * by key, get a typed value, and get a useful error when the field is missing
 * or has the wrong type. Values borrow the document's storage, so an
 * `idx_json_val` is only valid while its `idx_json_doc` is alive.
 */
#ifndef IDX_JSON_H
#define IDX_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bytes.h"
#include "error.h"

typedef struct idx_json_doc idx_json_doc;

/* A borrowed value. Passed by value; an absent value has `node == NULL`. */
typedef struct {
    const void *node;
} idx_json_val;

/* --------------------------------------------------------------- parsing -- */

/* Parses a copy of `input`. The input may be freed as soon as this returns. */
idx_status idx_json_parse(idx_slice input, idx_json_doc **out, idx_error *err);

/*
 * Parses in place, which avoids copying the multi-megabyte block payloads.
 * `data` is modified and must remain alive and untouched until the document is
 * freed. `capacity` must be at least `len + 4` bytes, because the parser needs
 * padding room past the end of the text.
 */
idx_status idx_json_parse_insitu(char *data, size_t len, size_t capacity,
                                 idx_json_doc **out, idx_error *err);

void idx_json_free(idx_json_doc *doc);

idx_json_val idx_json_root(const idx_json_doc *doc);

/* ------------------------------------------------------------ inspection -- */

bool idx_json_is_present(idx_json_val value); /* false for an absent value */
bool idx_json_is_null(idx_json_val value);    /* true for JSON null */
bool idx_json_is_object(idx_json_val value);
bool idx_json_is_array(idx_json_val value);
bool idx_json_is_string(idx_json_val value);
bool idx_json_is_number(idx_json_val value);
bool idx_json_is_bool(idx_json_val value);

/* Name of the value's type, for error messages. Never returns NULL. */
const char *idx_json_type_name(idx_json_val value);

/* ----------------------------------------------------------- navigation -- */

/* Absent keys yield an absent value rather than an error. */
idx_json_val idx_json_get(idx_json_val object, const char *key);

size_t idx_json_array_size(idx_json_val array);
idx_json_val idx_json_array_get(idx_json_val array, size_t index);

/*
 * Iterates an array:
 *
 *     size_t cursor = 0;
 *     idx_json_val item;
 *     while (idx_json_array_next(array, &cursor, &item)) { ... }
 */
bool idx_json_array_next(idx_json_val array, size_t *cursor,
                         idx_json_val *out);

/*
 * Iterates an object's members. `key` borrows the document's storage.
 */
bool idx_json_object_next(idx_json_val object, size_t *cursor, idx_slice *key,
                          idx_json_val *out);

/* ---------------------------------------------------------- typed reads -- */

/*
 * Each reader takes the containing object and a key so the error can name the
 * field. `field` is used only in messages and may be any label.
 */
idx_status idx_json_read_u64(idx_json_val object, const char *key,
                             uint64_t *out, idx_error *err);
idx_status idx_json_read_i64(idx_json_val object, const char *key, int64_t *out,
                             idx_error *err);
idx_status idx_json_read_double(idx_json_val object, const char *key,
                                double *out, idx_error *err);
idx_status idx_json_read_bool(idx_json_val object, const char *key, bool *out,
                              idx_error *err);

/* The slice borrows the document and is NUL-terminated at `data[len]`. */
idx_status idx_json_read_string(idx_json_val object, const char *key,
                                idx_slice *out, idx_error *err);

idx_status idx_json_read_object(idx_json_val object, const char *key,
                                idx_json_val *out, idx_error *err);
idx_status idx_json_read_array(idx_json_val object, const char *key,
                               idx_json_val *out, idx_error *err);

/*
 * Optional variants: return false when the field is absent or JSON null,
 * leaving `*out` untouched. A present field of the wrong type still fails, so
 * these are for optional fields, not for tolerating malformed data.
 */
bool idx_json_opt_u64(idx_json_val object, const char *key, uint64_t *out);
bool idx_json_opt_string(idx_json_val object, const char *key, idx_slice *out);
bool idx_json_opt_bool(idx_json_val object, const char *key, bool *out);

/* Direct conversion of a value already in hand. */
idx_status idx_json_as_u64(idx_json_val value, const char *what, uint64_t *out,
                           idx_error *err);
idx_status idx_json_as_string(idx_json_val value, const char *what,
                              idx_slice *out, idx_error *err);

/* --------------------------------------------------------------- writing -- */

/*
 * Appends `text` as a quoted, escaped JSON string. Requests are small and few,
 * so there is no document builder; anything more elaborate is assembled by the
 * caller from fragments.
 */
idx_status idx_json_write_escaped(idx_buffer *out, idx_slice text,
                                  idx_error *err);

/*
 * Builds a JSON-RPC 2.0 request. `params_json` is inserted verbatim and must
 * already be valid JSON (an array or object), or NULL to omit the field.
 */
idx_status idx_json_write_rpc_request(idx_buffer *out, uint64_t id,
                                      const char *method,
                                      const char *params_json, idx_error *err);

#endif /* IDX_JSON_H */
