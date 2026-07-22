/*
 * Growable array of fixed-size elements.
 *
 * Elements are stored by value and copied on push, so `idx_vec` owns its
 * contents but not anything they point to. Pointers returned by
 * `idx_vec_at` are invalidated by any operation that can grow the array.
 */
#ifndef IDX_VEC_H
#define IDX_VEC_H

#include <stdbool.h>
#include <stddef.h>

#include "error.h"

typedef struct {
    void *data;
    size_t len;
    size_t capacity;
    size_t elem_size;
} idx_vec;

/* `elem_size` must be non-zero. No allocation happens until the first push. */
void idx_vec_init(idx_vec *vec, size_t elem_size);

/* Ensures room for `additional` more elements beyond the current length. */
idx_status idx_vec_reserve(idx_vec *vec, size_t additional, idx_error *err);

/* Copies `elem_size` bytes from `elem` onto the end. */
idx_status idx_vec_push(idx_vec *vec, const void *elem, idx_error *err);

/*
 * Appends an uninitialized element and hands back a pointer to it, so callers
 * can construct in place instead of building a temporary first.
 */
idx_status idx_vec_push_uninit(idx_vec *vec, void **out, idx_error *err);

/* Returns NULL when `index` is out of range. */
void *idx_vec_at(const idx_vec *vec, size_t index);

/* Copies the last element into `out` (when not NULL) and removes it. */
bool idx_vec_pop(idx_vec *vec, void *out);

size_t idx_vec_len(const idx_vec *vec);
bool idx_vec_is_empty(const idx_vec *vec);

/* Drops the contents but keeps the allocation for reuse. */
void idx_vec_clear(idx_vec *vec);

void idx_vec_free(idx_vec *vec);

#endif /* IDX_VEC_H */
