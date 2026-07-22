#include "map.h"

#include <stdlib.h>
#include <string.h>

#define IDX_MAP_MIN_CAPACITY 16

/* Grow once three quarters of the slots are in use or spent on tombstones. */
#define IDX_MAP_LOAD_NUMERATOR 3
#define IDX_MAP_LOAD_DENOMINATOR 4

enum {
    SLOT_EMPTY = 0,
    SLOT_OCCUPIED = 1,
    SLOT_TOMBSTONE = 2
};

/* FNV-1a, 64 bit. Adequate for keys that are already hash-like. */
static uint64_t hash_bytes(idx_slice key) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < key.len; i++) {
        hash ^= key.data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static const uint8_t *entry_key(const idx_map_entry *entry) {
    return (entry->key_len > IDX_MAP_INLINE_KEY) ? entry->heap_key
                                                 : entry->inline_key;
}

static bool entry_matches(const idx_map_entry *entry, idx_slice key,
                          uint64_t hash) {
    if (entry->state != SLOT_OCCUPIED || entry->hash != hash ||
        entry->key_len != key.len) {
        return false;
    }
    if (key.len == 0) {
        return true;
    }
    return memcmp(entry_key(entry), key.data, key.len) == 0;
}

static void entry_release_key(idx_map_entry *entry) {
    if (entry->key_len > IDX_MAP_INLINE_KEY) {
        free(entry->heap_key);
    }
    entry->heap_key = NULL;
}

static idx_status entry_store_key(idx_map_entry *entry, idx_slice key,
                                  idx_error *err) {
    entry->key_len = key.len;
    entry->heap_key = NULL;

    if (key.len > IDX_MAP_INLINE_KEY) {
        entry->heap_key = malloc(key.len);
        if (entry->heap_key == NULL) {
            return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                            "failed to copy a %zu byte map key", key.len);
        }
        memcpy(entry->heap_key, key.data, key.len);
    } else if (key.len > 0) {
        memcpy(entry->inline_key, key.data, key.len);
    }
    return IDX_OK;
}

void idx_map_init(idx_map *map) {
    if (map == NULL) {
        return;
    }
    map->entries = NULL;
    map->capacity = 0;
    map->count = 0;
    map->tombstones = 0;
}

/*
 * Finds the slot holding `key`, or the slot it should be inserted into.
 * Insertion reuses the first tombstone seen, but only after confirming the key
 * is not present further along the probe sequence.
 */
static idx_map_entry *find_slot(const idx_map *map, idx_slice key,
                                uint64_t hash, bool for_insert) {
    if (map->capacity == 0) {
        return NULL;
    }

    size_t mask = map->capacity - 1;
    size_t index = (size_t)hash & mask;
    idx_map_entry *first_tombstone = NULL;

    for (size_t probe = 0; probe < map->capacity; probe++) {
        idx_map_entry *entry = &map->entries[index];

        if (entry->state == SLOT_EMPTY) {
            if (for_insert) {
                return (first_tombstone != NULL) ? first_tombstone : entry;
            }
            return NULL;
        }
        if (entry->state == SLOT_TOMBSTONE) {
            if (first_tombstone == NULL) {
                first_tombstone = entry;
            }
        } else if (entry_matches(entry, key, hash)) {
            return entry;
        }

        index = (index + 1) & mask;
    }

    return for_insert ? first_tombstone : NULL;
}

static idx_status rehash(idx_map *map, size_t new_capacity, idx_error *err) {
    idx_map_entry *entries = calloc(new_capacity, sizeof(*entries));
    if (entries == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to allocate a map of %zu slots", new_capacity);
    }

    idx_map rebuilt;
    rebuilt.entries = entries;
    rebuilt.capacity = new_capacity;
    rebuilt.count = 0;
    rebuilt.tombstones = 0;

    /*
     * Entries move wholesale, so heap keys are transferred rather than copied
     * and no allocation can fail partway through.
     */
    for (size_t i = 0; i < map->capacity; i++) {
        idx_map_entry *old = &map->entries[i];
        if (old->state != SLOT_OCCUPIED) {
            continue;
        }
        idx_slice key = idx_slice_make(entry_key(old), old->key_len);
        idx_map_entry *slot = find_slot(&rebuilt, key, old->hash, true);
        *slot = *old;
        rebuilt.count++;
    }

    free(map->entries);
    map->entries = rebuilt.entries;
    map->capacity = rebuilt.capacity;
    map->count = rebuilt.count;
    map->tombstones = 0;
    return IDX_OK;
}

static idx_status ensure_room(idx_map *map, idx_error *err) {
    if (map->capacity == 0) {
        return rehash(map, IDX_MAP_MIN_CAPACITY, err);
    }
    size_t used = map->count + map->tombstones;
    if (used * IDX_MAP_LOAD_DENOMINATOR <
        map->capacity * IDX_MAP_LOAD_NUMERATOR) {
        return IDX_OK;
    }
    /*
     * Tombstone-heavy tables are rebuilt at the same size; only genuine
     * occupancy doubles the table.
     */
    size_t target = map->capacity;
    if (map->count * IDX_MAP_LOAD_DENOMINATOR >=
        map->capacity * IDX_MAP_LOAD_NUMERATOR) {
        if (target > SIZE_MAX / 2) {
            return IDX_FAIL(err, IDX_ERR_RANGE, "map capacity overflows");
        }
        target *= 2;
    }
    return rehash(map, target, err);
}

idx_status idx_map_put(idx_map *map, idx_slice key, void *value,
                       idx_error *err) {
    if (map == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "map must not be NULL");
    }
    IDX_TRY(ensure_room(map, err));

    uint64_t hash = hash_bytes(key);
    idx_map_entry *slot = find_slot(map, key, hash, true);
    if (slot == NULL) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL, "map has no free slot");
    }

    if (slot->state == SLOT_OCCUPIED) {
        slot->value = value;
        return IDX_OK;
    }

    if (slot->state == SLOT_TOMBSTONE) {
        map->tombstones--;
    }

    IDX_TRY(entry_store_key(slot, key, err));
    slot->state = SLOT_OCCUPIED;
    slot->hash = hash;
    slot->value = value;
    map->count++;
    return IDX_OK;
}

bool idx_map_get(const idx_map *map, idx_slice key, void **out_value) {
    if (map == NULL) {
        return false;
    }
    idx_map_entry *slot = find_slot(map, key, hash_bytes(key), false);
    if (slot == NULL) {
        return false;
    }
    if (out_value != NULL) {
        *out_value = slot->value;
    }
    return true;
}

bool idx_map_contains(const idx_map *map, idx_slice key) {
    return idx_map_get(map, key, NULL);
}

bool idx_map_remove(idx_map *map, idx_slice key) {
    if (map == NULL) {
        return false;
    }
    idx_map_entry *slot = find_slot(map, key, hash_bytes(key), false);
    if (slot == NULL) {
        return false;
    }

    entry_release_key(slot);
    slot->state = SLOT_TOMBSTONE;
    slot->key_len = 0;
    slot->value = NULL;
    map->count--;
    map->tombstones++;
    return true;
}

size_t idx_map_count(const idx_map *map) {
    return (map != NULL) ? map->count : 0;
}

void idx_map_clear(idx_map *map) {
    if (map == NULL || map->entries == NULL) {
        return;
    }
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].state == SLOT_OCCUPIED) {
            entry_release_key(&map->entries[i]);
        }
    }
    memset(map->entries, 0, map->capacity * sizeof(*map->entries));
    map->count = 0;
    map->tombstones = 0;
}

void idx_map_free(idx_map *map) {
    if (map == NULL) {
        return;
    }
    idx_map_clear(map);
    free(map->entries);
    idx_map_init(map);
}

bool idx_map_next(const idx_map *map, size_t *cursor, idx_slice *out_key,
                  void **out_value) {
    if (map == NULL || cursor == NULL) {
        return false;
    }
    for (size_t i = *cursor; i < map->capacity; i++) {
        idx_map_entry *entry = &map->entries[i];
        if (entry->state != SLOT_OCCUPIED) {
            continue;
        }
        if (out_key != NULL) {
            *out_key = idx_slice_make(entry_key(entry), entry->key_len);
        }
        if (out_value != NULL) {
            *out_value = entry->value;
        }
        *cursor = i + 1;
        return true;
    }
    return false;
}
