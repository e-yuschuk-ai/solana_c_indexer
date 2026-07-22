/*
 * Bump allocator with chunked backing storage.
 *
 * Blocks and transactions are decoded into short-lived object graphs whose
 * lifetime ends when the unit of work does. Instead of freeing each node, the
 * pipeline allocates from an arena and calls idx_arena_reset() at the end of
 * the block: reset keeps the chunks around so steady-state processing performs
 * no allocator calls at all.
 *
 * An arena is not thread-safe. Give each worker its own.
 */
#ifndef IDX_ARENA_H
#define IDX_ARENA_H

#include <stddef.h>

#include "error.h"

#define IDX_ARENA_DEFAULT_CHUNK_SIZE (64u * 1024u)

typedef struct idx_arena_chunk idx_arena_chunk;

typedef struct {
    idx_arena_chunk *first;   /* oldest chunk; reset rewinds here */
    idx_arena_chunk *current; /* chunk being filled */
    size_t chunk_size;        /* size used for newly grown chunks */
    size_t bytes_reserved;    /* total capacity held by the arena */
    size_t bytes_used;        /* consumed since the last reset, incl. padding */
} idx_arena;

/*
 * Prepares an arena. No memory is reserved until the first allocation.
 * `chunk_size` of 0 selects IDX_ARENA_DEFAULT_CHUNK_SIZE.
 */
void idx_arena_init(idx_arena *arena, size_t chunk_size);

/* Allocates `size` bytes aligned for any scalar type. */
idx_status idx_arena_alloc(idx_arena *arena, size_t size, void **out,
                           idx_error *err);

/* As idx_arena_alloc, with an explicit power-of-two alignment. */
idx_status idx_arena_alloc_aligned(idx_arena *arena, size_t size, size_t align,
                                   void **out, idx_error *err);

/* Allocates `count * size` zeroed bytes, checking the product for overflow. */
idx_status idx_arena_calloc(idx_arena *arena, size_t count, size_t size,
                            void **out, idx_error *err);

/* Copies a NUL-terminated string into the arena. */
idx_status idx_arena_strdup(idx_arena *arena, const char *str, char **out,
                            idx_error *err);

/*
 * Releases every allocation while retaining the chunks for reuse. Pointers
 * handed out before the reset must not be used afterwards.
 */
void idx_arena_reset(idx_arena *arena);

/* Frees all chunks. The arena may be used again; it starts empty. */
void idx_arena_destroy(idx_arena *arena);

#endif /* IDX_ARENA_H */
