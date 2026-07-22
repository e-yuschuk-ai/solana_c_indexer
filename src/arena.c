#include "arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct idx_arena_chunk {
    idx_arena_chunk *next;
    size_t capacity;
    size_t used;
    _Alignas(max_align_t) unsigned char data[];
};

void idx_arena_init(idx_arena *arena, size_t chunk_size) {
    if (arena == NULL) {
        return;
    }
    arena->first = NULL;
    arena->current = NULL;
    arena->chunk_size =
        (chunk_size > 0) ? chunk_size : IDX_ARENA_DEFAULT_CHUNK_SIZE;
    arena->bytes_reserved = 0;
    arena->bytes_used = 0;
}

static int is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

/*
 * Padding needed so that `chunk->data + chunk->used` reaches an `align`
 * boundary. Computed on the real address rather than the offset, so it holds
 * regardless of how the chunk header is laid out.
 */
static size_t padding_for(const idx_arena_chunk *chunk, size_t align) {
    uintptr_t cursor = (uintptr_t)chunk->data + chunk->used;
    uintptr_t aligned = (cursor + (uintptr_t)align - 1) & ~((uintptr_t)align - 1);
    return (size_t)(aligned - cursor);
}

static int chunk_fits(const idx_arena_chunk *chunk, size_t size, size_t align) {
    size_t padding = padding_for(chunk, align);
    if (padding > chunk->capacity - chunk->used) {
        return 0;
    }
    return size <= chunk->capacity - chunk->used - padding;
}

static idx_status chunk_new(idx_arena *arena, size_t size, size_t align,
                            idx_arena_chunk **out, idx_error *err) {
    /* Guarantee the request fits even in the worst-case alignment position. */
    size_t needed = size;
    if (needed > SIZE_MAX - (align - 1)) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "arena allocation of %zu bytes is too large", size);
    }
    needed += align - 1;

    size_t capacity = (needed > arena->chunk_size) ? needed : arena->chunk_size;
    if (capacity > SIZE_MAX - sizeof(idx_arena_chunk)) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "arena chunk of %zu bytes is too large", capacity);
    }

    idx_arena_chunk *chunk = malloc(sizeof(idx_arena_chunk) + capacity);
    if (chunk == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to reserve an arena chunk of %zu bytes",
                        capacity);
    }

    chunk->next = NULL;
    chunk->capacity = capacity;
    chunk->used = 0;

    arena->bytes_reserved += capacity;
    *out = chunk;
    return IDX_OK;
}

idx_status idx_arena_alloc_aligned(idx_arena *arena, size_t size, size_t align,
                                   void **out, idx_error *err) {
    if (arena == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "arena and out must not be NULL");
    }
    if (!is_power_of_two(align)) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "alignment %zu is not a power of two", align);
    }

    /*
     * Chunks past `current` exist only after a reset, when they are empty and
     * ready for reuse. Walk them before growing the arena.
     */
    for (idx_arena_chunk *chunk = arena->current; chunk != NULL;
         chunk = chunk->next) {
        if (chunk_fits(chunk, size, align)) {
            size_t padding = padding_for(chunk, align);
            unsigned char *ptr = chunk->data + chunk->used + padding;
            chunk->used += padding + size;
            arena->current = chunk;
            arena->bytes_used += padding + size;
            *out = ptr;
            return IDX_OK;
        }
    }

    idx_arena_chunk *chunk = NULL;
    IDX_TRY(chunk_new(arena, size, align, &chunk, err));

    if (arena->first == NULL) {
        arena->first = chunk;
    } else {
        idx_arena_chunk *tail = arena->current;
        if (tail == NULL) {
            tail = arena->first;
        }
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = chunk;
    }
    arena->current = chunk;

    size_t padding = padding_for(chunk, align);
    unsigned char *ptr = chunk->data + padding;
    chunk->used = padding + size;
    arena->bytes_used += padding + size;
    *out = ptr;
    return IDX_OK;
}

idx_status idx_arena_alloc(idx_arena *arena, size_t size, void **out,
                           idx_error *err) {
    return idx_arena_alloc_aligned(arena, size, _Alignof(max_align_t), out, err);
}

idx_status idx_arena_calloc(idx_arena *arena, size_t count, size_t size,
                            void **out, idx_error *err) {
    if (count != 0 && size > SIZE_MAX / count) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "arena allocation of %zu * %zu bytes overflows", count,
                        size);
    }
    size_t total = count * size;
    IDX_TRY(idx_arena_alloc(arena, total, out, err));
    memset(*out, 0, total);
    return IDX_OK;
}

idx_status idx_arena_strdup(idx_arena *arena, const char *str, char **out,
                            idx_error *err) {
    if (str == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "str and out must not be NULL");
    }
    size_t size = strlen(str) + 1;
    void *ptr = NULL;
    IDX_TRY(idx_arena_alloc_aligned(arena, size, 1, &ptr, err));
    memcpy(ptr, str, size);
    *out = ptr;
    return IDX_OK;
}

void idx_arena_reset(idx_arena *arena) {
    if (arena == NULL) {
        return;
    }
    for (idx_arena_chunk *chunk = arena->first; chunk != NULL;
         chunk = chunk->next) {
        chunk->used = 0;
    }
    arena->current = arena->first;
    arena->bytes_used = 0;
}

void idx_arena_destroy(idx_arena *arena) {
    if (arena == NULL) {
        return;
    }
    idx_arena_chunk *chunk = arena->first;
    while (chunk != NULL) {
        idx_arena_chunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    size_t chunk_size = arena->chunk_size;
    idx_arena_init(arena, chunk_size);
}
