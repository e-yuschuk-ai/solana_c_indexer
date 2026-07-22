#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson/yyjson.h"

struct idx_json_doc {
    yyjson_doc *doc;
};

static yyjson_val *node_of(idx_json_val value) {
    return (yyjson_val *)value.node;
}

static idx_json_val wrap(yyjson_val *node) {
    idx_json_val value;
    value.node = node;
    return value;
}

/* --------------------------------------------------------------- parsing -- */

static idx_status parse_failure(const yyjson_read_err *read_err,
                                idx_error *err) {
    return IDX_FAIL(err, IDX_ERR_PARSE, "invalid JSON at offset %zu: %s",
                    read_err->pos,
                    (read_err->msg != NULL) ? read_err->msg : "unknown");
}

idx_status idx_json_parse(idx_slice input, idx_json_doc **out, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }

    yyjson_read_err read_err;
    memset(&read_err, 0, sizeof(read_err));

    yyjson_doc *doc = yyjson_read_opts((char *)(uintptr_t)input.data, input.len,
                                       0, NULL, &read_err);
    if (doc == NULL) {
        return parse_failure(&read_err, err);
    }

    idx_json_doc *wrapper = malloc(sizeof(*wrapper));
    if (wrapper == NULL) {
        yyjson_doc_free(doc);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to allocate a JSON document handle");
    }
    wrapper->doc = doc;
    *out = wrapper;
    return IDX_OK;
}

idx_status idx_json_parse_insitu(char *data, size_t len, size_t capacity,
                                 idx_json_doc **out, idx_error *err) {
    if (data == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "data and out must not be NULL");
    }
    /* yyjson writes a terminator past the text while parsing in place. */
    if (capacity < len + 4) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "in-situ parse needs %zu bytes of capacity, got %zu",
                        len + 4, capacity);
    }

    yyjson_read_err read_err;
    memset(&read_err, 0, sizeof(read_err));

    yyjson_doc *doc =
        yyjson_read_opts(data, len, YYJSON_READ_INSITU, NULL, &read_err);
    if (doc == NULL) {
        return parse_failure(&read_err, err);
    }

    idx_json_doc *wrapper = malloc(sizeof(*wrapper));
    if (wrapper == NULL) {
        yyjson_doc_free(doc);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to allocate a JSON document handle");
    }
    wrapper->doc = doc;
    *out = wrapper;
    return IDX_OK;
}

void idx_json_free(idx_json_doc *doc) {
    if (doc == NULL) {
        return;
    }
    yyjson_doc_free(doc->doc);
    free(doc);
}

idx_json_val idx_json_root(const idx_json_doc *doc) {
    if (doc == NULL) {
        return wrap(NULL);
    }
    return wrap(yyjson_doc_get_root(doc->doc));
}

/* ------------------------------------------------------------ inspection -- */

bool idx_json_is_present(idx_json_val value) { return value.node != NULL; }

bool idx_json_is_null(idx_json_val value) {
    return value.node != NULL && yyjson_is_null(node_of(value));
}

bool idx_json_is_object(idx_json_val value) {
    return value.node != NULL && yyjson_is_obj(node_of(value));
}

bool idx_json_is_array(idx_json_val value) {
    return value.node != NULL && yyjson_is_arr(node_of(value));
}

bool idx_json_is_string(idx_json_val value) {
    return value.node != NULL && yyjson_is_str(node_of(value));
}

bool idx_json_is_number(idx_json_val value) {
    return value.node != NULL && yyjson_is_num(node_of(value));
}

bool idx_json_is_bool(idx_json_val value) {
    return value.node != NULL && yyjson_is_bool(node_of(value));
}

const char *idx_json_type_name(idx_json_val value) {
    if (value.node == NULL) {
        return "absent";
    }
    yyjson_val *node = node_of(value);
    if (yyjson_is_null(node)) {
        return "null";
    }
    if (yyjson_is_bool(node)) {
        return "boolean";
    }
    if (yyjson_is_num(node)) {
        return "number";
    }
    if (yyjson_is_str(node)) {
        return "string";
    }
    if (yyjson_is_arr(node)) {
        return "array";
    }
    if (yyjson_is_obj(node)) {
        return "object";
    }
    return "unknown";
}

/* ----------------------------------------------------------- navigation -- */

idx_json_val idx_json_get(idx_json_val object, const char *key) {
    if (object.node == NULL || key == NULL ||
        !yyjson_is_obj(node_of(object))) {
        return wrap(NULL);
    }
    return wrap(yyjson_obj_get(node_of(object), key));
}

size_t idx_json_array_size(idx_json_val array) {
    if (!idx_json_is_array(array)) {
        return 0;
    }
    return yyjson_arr_size(node_of(array));
}

idx_json_val idx_json_array_get(idx_json_val array, size_t index) {
    if (!idx_json_is_array(array)) {
        return wrap(NULL);
    }
    return wrap(yyjson_arr_get(node_of(array), index));
}

bool idx_json_array_next(idx_json_val array, size_t *cursor,
                         idx_json_val *out) {
    if (cursor == NULL || !idx_json_is_array(array)) {
        return false;
    }
    yyjson_val *item = yyjson_arr_get(node_of(array), *cursor);
    if (item == NULL) {
        return false;
    }
    if (out != NULL) {
        *out = wrap(item);
    }
    (*cursor)++;
    return true;
}

bool idx_json_object_next(idx_json_val object, size_t *cursor, idx_slice *key,
                          idx_json_val *out) {
    if (cursor == NULL || !idx_json_is_object(object)) {
        return false;
    }

    yyjson_obj_iter iter;
    if (!yyjson_obj_iter_init(node_of(object), &iter)) {
        return false;
    }

    /*
     * yyjson's iterator is sequential, so reaching index `*cursor` means
     * stepping to it. Object members in an RPC response are few, and callers
     * that need more than a linear walk should reach in by key instead.
     */
    yyjson_val *found_key = NULL;
    for (size_t i = 0; i <= *cursor; i++) {
        found_key = yyjson_obj_iter_next(&iter);
        if (found_key == NULL) {
            return false;
        }
    }

    if (key != NULL) {
        *key = idx_slice_make(yyjson_get_str(found_key),
                              yyjson_get_len(found_key));
    }
    if (out != NULL) {
        *out = wrap(yyjson_obj_iter_get_val(found_key));
    }
    (*cursor)++;
    return true;
}

/* ---------------------------------------------------------- typed reads -- */

static idx_status missing(const char *key, idx_error *err) {
    return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "missing field '%s'",
                    (key != NULL) ? key : "?");
}

static idx_status wrong_type(const char *key, const char *expected,
                             idx_json_val actual, idx_error *err) {
    return IDX_FAIL(err, IDX_ERR_PARSE, "field '%s' is %s, expected %s",
                    (key != NULL) ? key : "?", idx_json_type_name(actual),
                    expected);
}

idx_status idx_json_as_u64(idx_json_val value, const char *what, uint64_t *out,
                           idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    if (!idx_json_is_present(value)) {
        return missing(what, err);
    }
    yyjson_val *node = node_of(value);

    if (yyjson_is_uint(node)) {
        *out = yyjson_get_uint(node);
        return IDX_OK;
    }
    if (yyjson_is_sint(node)) {
        int64_t signed_value = yyjson_get_sint(node);
        if (signed_value < 0) {
            return IDX_FAIL(err, IDX_ERR_RANGE,
                            "field '%s' is negative (%lld), expected unsigned",
                            (what != NULL) ? what : "?",
                            (long long)signed_value);
        }
        *out = (uint64_t)signed_value;
        return IDX_OK;
    }
    return wrong_type(what, "an unsigned integer", value, err);
}

idx_status idx_json_read_u64(idx_json_val object, const char *key,
                             uint64_t *out, idx_error *err) {
    return idx_json_as_u64(idx_json_get(object, key), key, out, err);
}

idx_status idx_json_read_i64(idx_json_val object, const char *key, int64_t *out,
                             idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    idx_json_val value = idx_json_get(object, key);
    if (!idx_json_is_present(value)) {
        return missing(key, err);
    }
    yyjson_val *node = node_of(value);

    if (yyjson_is_sint(node) || yyjson_is_uint(node)) {
        *out = yyjson_get_sint(node);
        return IDX_OK;
    }
    return wrong_type(key, "an integer", value, err);
}

idx_status idx_json_read_double(idx_json_val object, const char *key,
                                double *out, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    idx_json_val value = idx_json_get(object, key);
    if (!idx_json_is_present(value)) {
        return missing(key, err);
    }
    if (!idx_json_is_number(value)) {
        return wrong_type(key, "a number", value, err);
    }
    *out = yyjson_get_num(node_of(value));
    return IDX_OK;
}

idx_status idx_json_read_bool(idx_json_val object, const char *key, bool *out,
                              idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    idx_json_val value = idx_json_get(object, key);
    if (!idx_json_is_present(value)) {
        return missing(key, err);
    }
    if (!idx_json_is_bool(value)) {
        return wrong_type(key, "a boolean", value, err);
    }
    *out = yyjson_get_bool(node_of(value));
    return IDX_OK;
}

idx_status idx_json_as_string(idx_json_val value, const char *what,
                              idx_slice *out, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    if (!idx_json_is_present(value)) {
        return missing(what, err);
    }
    if (!idx_json_is_string(value)) {
        return wrong_type(what, "a string", value, err);
    }
    yyjson_val *node = node_of(value);
    *out = idx_slice_make(yyjson_get_str(node), yyjson_get_len(node));
    return IDX_OK;
}

idx_status idx_json_read_string(idx_json_val object, const char *key,
                                idx_slice *out, idx_error *err) {
    return idx_json_as_string(idx_json_get(object, key), key, out, err);
}

idx_status idx_json_read_object(idx_json_val object, const char *key,
                                idx_json_val *out, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    idx_json_val value = idx_json_get(object, key);
    if (!idx_json_is_present(value)) {
        return missing(key, err);
    }
    if (!idx_json_is_object(value)) {
        return wrong_type(key, "an object", value, err);
    }
    *out = value;
    return IDX_OK;
}

idx_status idx_json_read_array(idx_json_val object, const char *key,
                               idx_json_val *out, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    idx_json_val value = idx_json_get(object, key);
    if (!idx_json_is_present(value)) {
        return missing(key, err);
    }
    if (!idx_json_is_array(value)) {
        return wrong_type(key, "an array", value, err);
    }
    *out = value;
    return IDX_OK;
}

bool idx_json_opt_u64(idx_json_val object, const char *key, uint64_t *out) {
    idx_json_val value = idx_json_get(object, key);
    if (!idx_json_is_present(value) || idx_json_is_null(value)) {
        return false;
    }
    return idx_json_as_u64(value, key, out, NULL) == IDX_OK;
}

bool idx_json_opt_string(idx_json_val object, const char *key, idx_slice *out) {
    idx_json_val value = idx_json_get(object, key);
    if (!idx_json_is_present(value) || idx_json_is_null(value)) {
        return false;
    }
    return idx_json_as_string(value, key, out, NULL) == IDX_OK;
}

bool idx_json_opt_bool(idx_json_val object, const char *key, bool *out) {
    idx_json_val value = idx_json_get(object, key);
    if (!idx_json_is_present(value) || idx_json_is_null(value) ||
        !idx_json_is_bool(value)) {
        return false;
    }
    if (out != NULL) {
        *out = yyjson_get_bool(node_of(value));
    }
    return true;
}

/* --------------------------------------------------------------- writing -- */

idx_status idx_json_write_escaped(idx_buffer *out, idx_slice text,
                                  idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }

    IDX_TRY(idx_buffer_append_byte(out, '"', err));

    for (size_t i = 0; i < text.len; i++) {
        uint8_t c = text.data[i];
        switch (c) {
            case '"':
                IDX_TRY(idx_buffer_append(out, "\\\"", 2, err));
                break;
            case '\\':
                IDX_TRY(idx_buffer_append(out, "\\\\", 2, err));
                break;
            case '\n':
                IDX_TRY(idx_buffer_append(out, "\\n", 2, err));
                break;
            case '\r':
                IDX_TRY(idx_buffer_append(out, "\\r", 2, err));
                break;
            case '\t':
                IDX_TRY(idx_buffer_append(out, "\\t", 2, err));
                break;
            case '\b':
                IDX_TRY(idx_buffer_append(out, "\\b", 2, err));
                break;
            case '\f':
                IDX_TRY(idx_buffer_append(out, "\\f", 2, err));
                break;
            default:
                if (c < 0x20) {
                    char escape[7];
                    snprintf(escape, sizeof(escape), "\\u%04x", c);
                    IDX_TRY(idx_buffer_append(out, escape, 6, err));
                } else {
                    /* Bytes above 0x7F pass through: the input is UTF-8 and
                     * JSON accepts it unescaped. */
                    IDX_TRY(idx_buffer_append_byte(out, c, err));
                }
                break;
        }
    }

    return idx_buffer_append_byte(out, '"', err);
}

idx_status idx_json_write_rpc_request(idx_buffer *out, uint64_t id,
                                      const char *method,
                                      const char *params_json, idx_error *err) {
    if (out == NULL || method == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "out and method must not be NULL");
    }

    char prefix[64];
    int written = snprintf(prefix, sizeof(prefix),
                           "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"method\":",
                           (unsigned long long)id);
    if (written < 0 || (size_t)written >= sizeof(prefix)) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL,
                        "failed to format the JSON-RPC prefix");
    }

    IDX_TRY(idx_buffer_append(out, prefix, (size_t)written, err));
    IDX_TRY(idx_json_write_escaped(out, idx_slice_from_str(method), err));

    if (params_json != NULL) {
        IDX_TRY(idx_buffer_append(out, ",\"params\":", 10, err));
        IDX_TRY(idx_buffer_append(out, params_json, strlen(params_json), err));
    }

    return idx_buffer_append_byte(out, '}', err);
}
