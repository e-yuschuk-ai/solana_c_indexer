/*
 * Hash map with byte-string keys and pointer-sized values.
 *
 * Open addressing with linear probing. Keys are copied into the map; those up
 * to IDX_MAP_INLINE_KEY bytes are stored inside the entry, which covers the
 * dominant case of a 32-byte pubkey and keeps the common path allocation-free.
 *
 * Not thread-safe.
 */
#ifndef IDX_MAP_H
#define IDX_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bytes.h"
#include "error.h"

#define IDX_MAP_INLINE_KEY 32

typedef struct {
    uint8_t state;
    uint64_t hash;
    size_t key_len;
    uint8_t inline_key[IDX_MAP_INLINE_KEY];
    uint8_t *heap_key; /* used only when key_len > IDX_MAP_INLINE_KEY */
    void *value;
} idx_map_entry;

typedef struct {
    idx_map_entry *entries;
    size_t capacity; /* always a power of two, or 0 before the first insert */
    size_t count;
    size_t tombstones;
} idx_map;

void idx_map_init(idx_map *map);

/* Inserts or replaces. The key is copied; the value is stored as given. */
idx_status idx_map_put(idx_map *map, idx_slice key, void *value,
                       idx_error *err);

/*
 * Looks up `key`. Returns false when absent, in which case `*out_value` is
 * untouched. `out_value` may be NULL to test for presence only.
 */
bool idx_map_get(const idx_map *map, idx_slice key, void **out_value);

bool idx_map_contains(const idx_map *map, idx_slice key);

/* Returns false when the key was absent. */
bool idx_map_remove(idx_map *map, idx_slice key);

size_t idx_map_count(const idx_map *map);

/* Drops every entry but keeps the table allocation. */
void idx_map_clear(idx_map *map);

void idx_map_free(idx_map *map);

/*
 * Iterates. Start with `*cursor` set to 0 and call until it returns false:
 *
 *     size_t cursor = 0;
 *     idx_slice key;
 *     void *value;
 *     while (idx_map_next(&map, &cursor, &key, &value)) { ... }
 *
 * The map must not be modified during iteration. Keys borrow the map's
 * storage and stay valid until the entry is removed or the map changes.
 */
bool idx_map_next(const idx_map *map, size_t *cursor, idx_slice *out_key,
                  void **out_value);

#endif /* IDX_MAP_H */
