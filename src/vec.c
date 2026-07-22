#include "vec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define IDX_VEC_MIN_CAPACITY 8

void idx_vec_init(idx_vec *vec, size_t elem_size) {
    if (vec == NULL) {
        return;
    }
    vec->data = NULL;
    vec->len = 0;
    vec->capacity = 0;
    vec->elem_size = elem_size;
}

static void *element_at(const idx_vec *vec, size_t index) {
    return (unsigned char *)vec->data + index * vec->elem_size;
}

idx_status idx_vec_reserve(idx_vec *vec, size_t additional, idx_error *err) {
    if (vec == NULL || vec->elem_size == 0) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "vec must be initialized with a non-zero elem_size");
    }
    if (additional <= vec->capacity - vec->len) {
        return IDX_OK;
    }
    if (additional > SIZE_MAX - vec->len) {
        return IDX_FAIL(err, IDX_ERR_RANGE, "vec growth of %zu overflows",
                        additional);
    }

    size_t needed = vec->len + additional;
    size_t capacity = (vec->capacity > 0) ? vec->capacity : IDX_VEC_MIN_CAPACITY;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }

    if (capacity > SIZE_MAX / vec->elem_size) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "vec of %zu elements of %zu bytes overflows", capacity,
                        vec->elem_size);
    }

    void *grown = realloc(vec->data, capacity * vec->elem_size);
    if (grown == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to grow vec to %zu elements", capacity);
    }
    vec->data = grown;
    vec->capacity = capacity;
    return IDX_OK;
}

idx_status idx_vec_push_uninit(idx_vec *vec, void **out, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    IDX_TRY(idx_vec_reserve(vec, 1, err));
    *out = element_at(vec, vec->len);
    vec->len++;
    return IDX_OK;
}

idx_status idx_vec_push(idx_vec *vec, const void *elem, idx_error *err) {
    if (elem == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "elem must not be NULL");
    }
    void *destination = NULL;
    IDX_TRY(idx_vec_push_uninit(vec, &destination, err));
    memcpy(destination, elem, vec->elem_size);
    return IDX_OK;
}

void *idx_vec_at(const idx_vec *vec, size_t index) {
    if (vec == NULL || index >= vec->len) {
        return NULL;
    }
    return element_at(vec, index);
}

bool idx_vec_pop(idx_vec *vec, void *out) {
    if (vec == NULL || vec->len == 0) {
        return false;
    }
    vec->len--;
    if (out != NULL) {
        memcpy(out, element_at(vec, vec->len), vec->elem_size);
    }
    return true;
}

size_t idx_vec_len(const idx_vec *vec) { return (vec != NULL) ? vec->len : 0; }

bool idx_vec_is_empty(const idx_vec *vec) { return idx_vec_len(vec) == 0; }

void idx_vec_clear(idx_vec *vec) {
    if (vec != NULL) {
        vec->len = 0;
    }
}

void idx_vec_free(idx_vec *vec) {
    if (vec == NULL) {
        return;
    }
    free(vec->data);
    size_t elem_size = vec->elem_size;
    idx_vec_init(vec, elem_size);
}
