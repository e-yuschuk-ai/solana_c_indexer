#include "map.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "types.h"

static idx_slice key_of(const char *text) { return idx_slice_from_str(text); }

static void test_put_and_get(void) {
    idx_map map;
    idx_map_init(&map);

    TEST_EQ_UINT(idx_map_count(&map), 0u);
    TEST_ASSERT(!idx_map_get(&map, key_of("absent"), NULL));

    int a = 1, b = 2;
    TEST_EQ_INT(idx_map_put(&map, key_of("alpha"), &a, NULL), IDX_OK);
    TEST_EQ_INT(idx_map_put(&map, key_of("beta"), &b, NULL), IDX_OK);
    TEST_EQ_UINT(idx_map_count(&map), 2u);

    void *value = NULL;
    TEST_ASSERT(idx_map_get(&map, key_of("alpha"), &value));
    TEST_ASSERT(value == &a);
    TEST_ASSERT(idx_map_get(&map, key_of("beta"), &value));
    TEST_ASSERT(value == &b);
    TEST_ASSERT(idx_map_contains(&map, key_of("alpha")));
    TEST_ASSERT(!idx_map_contains(&map, key_of("gamma")));

    /* A miss must leave the output untouched. */
    void *untouched = (void *)0x1234;
    TEST_ASSERT(!idx_map_get(&map, key_of("gamma"), &untouched));
    TEST_ASSERT(untouched == (void *)0x1234);

    idx_map_free(&map);
}

static void test_put_replaces(void) {
    idx_map map;
    idx_map_init(&map);

    int first = 1, second = 2;
    TEST_EQ_INT(idx_map_put(&map, key_of("key"), &first, NULL), IDX_OK);
    TEST_EQ_INT(idx_map_put(&map, key_of("key"), &second, NULL), IDX_OK);

    TEST_EQ_UINT(idx_map_count(&map), 1u);

    void *value = NULL;
    TEST_ASSERT(idx_map_get(&map, key_of("key"), &value));
    TEST_ASSERT(value == &second);

    idx_map_free(&map);
}

static void test_remove(void) {
    idx_map map;
    idx_map_init(&map);

    int value = 1;
    TEST_EQ_INT(idx_map_put(&map, key_of("gone"), &value, NULL), IDX_OK);
    TEST_ASSERT(idx_map_remove(&map, key_of("gone")));
    TEST_EQ_UINT(idx_map_count(&map), 0u);
    TEST_ASSERT(!idx_map_get(&map, key_of("gone"), NULL));

    /* Removing twice reports the second attempt as a miss. */
    TEST_ASSERT(!idx_map_remove(&map, key_of("gone")));
    TEST_ASSERT(!idx_map_remove(&map, key_of("never there")));

    idx_map_free(&map);
}

/*
 * Removal leaves tombstones. Keys inserted after a removal must still be
 * findable through the probe sequence the tombstone sits in.
 */
static void test_tombstones_do_not_break_probing(void) {
    idx_map map;
    idx_map_init(&map);

    enum { COUNT = 500 };
    static int values[COUNT];
    char key[32];

    for (int i = 0; i < COUNT; i++) {
        values[i] = i;
        snprintf(key, sizeof(key), "key-%d", i);
        TEST_EQ_INT(idx_map_put(&map, key_of(key), &values[i], NULL), IDX_OK);
    }

    /* Remove every other key, then look up the ones that remain. */
    for (int i = 0; i < COUNT; i += 2) {
        snprintf(key, sizeof(key), "key-%d", i);
        TEST_ASSERT(idx_map_remove(&map, key_of(key)));
    }
    TEST_EQ_UINT(idx_map_count(&map), COUNT / 2);

    int intact = 1;
    for (int i = 1; i < COUNT; i += 2) {
        snprintf(key, sizeof(key), "key-%d", i);
        void *value = NULL;
        if (!idx_map_get(&map, key_of(key), &value) || value != &values[i]) {
            intact = 0;
        }
    }
    TEST_ASSERT(intact);

    /* Re-inserting the removed keys must reuse the tombstones. */
    for (int i = 0; i < COUNT; i += 2) {
        snprintf(key, sizeof(key), "key-%d", i);
        TEST_EQ_INT(idx_map_put(&map, key_of(key), &values[i], NULL), IDX_OK);
    }
    TEST_EQ_UINT(idx_map_count(&map), COUNT);

    idx_map_free(&map);
}

/* Growth must rehash every entry, including heap-allocated long keys. */
static void test_growth(void) {
    idx_map map;
    idx_map_init(&map);

    enum { COUNT = 2000 };
    static int values[COUNT];
    char key[128];

    for (int i = 0; i < COUNT; i++) {
        values[i] = i;
        /* Alternate between inline-sized and heap-sized keys. */
        if (i % 2 == 0) {
            snprintf(key, sizeof(key), "short-%d", i);
        } else {
            snprintf(key, sizeof(key),
                     "a-very-long-key-that-will-not-fit-inline-%d-%s", i,
                     "padding-padding-padding");
        }
        TEST_EQ_INT(idx_map_put(&map, key_of(key), &values[i], NULL), IDX_OK);
    }

    TEST_EQ_UINT(idx_map_count(&map), COUNT);

    int intact = 1;
    for (int i = 0; i < COUNT; i++) {
        if (i % 2 == 0) {
            snprintf(key, sizeof(key), "short-%d", i);
        } else {
            snprintf(key, sizeof(key),
                     "a-very-long-key-that-will-not-fit-inline-%d-%s", i,
                     "padding-padding-padding");
        }
        void *value = NULL;
        if (!idx_map_get(&map, key_of(key), &value) || value != &values[i]) {
            intact = 0;
        }
    }
    TEST_ASSERT(intact);

    idx_map_free(&map);
}

/* Pubkeys are the intended key type and must fit inline. */
static void test_pubkey_keys(void) {
    idx_map map;
    idx_map_init(&map);

    TEST_ASSERT(IDX_PUBKEY_LEN <= IDX_MAP_INLINE_KEY);

    static int token = 1, system_program = 2;
    TEST_EQ_INT(idx_map_put(&map, idx_pubkey_slice(&IDX_PROGRAM_TOKEN), &token,
                            NULL),
                IDX_OK);
    TEST_EQ_INT(idx_map_put(&map, idx_pubkey_slice(&IDX_PROGRAM_SYSTEM),
                            &system_program, NULL),
                IDX_OK);

    void *value = NULL;
    TEST_ASSERT(idx_map_get(&map, idx_pubkey_slice(&IDX_PROGRAM_TOKEN), &value));
    TEST_ASSERT(value == &token);
    TEST_ASSERT(
        !idx_map_get(&map, idx_pubkey_slice(&IDX_PROGRAM_TOKEN_2022), &value));

    idx_map_free(&map);
}

/* Keys are copied, so the caller's buffer may be reused or go out of scope. */
static void test_keys_are_copied(void) {
    idx_map map;
    idx_map_init(&map);

    int value = 1;
    char scratch[16];
    strcpy(scratch, "original");
    TEST_EQ_INT(idx_map_put(&map, key_of(scratch), &value, NULL), IDX_OK);

    memset(scratch, 'x', sizeof(scratch));
    TEST_ASSERT(idx_map_get(&map, key_of("original"), NULL));

    idx_map_free(&map);
}

static void test_empty_key(void) {
    idx_map map;
    idx_map_init(&map);

    int value = 1;
    TEST_EQ_INT(idx_map_put(&map, idx_slice_make(NULL, 0), &value, NULL),
                IDX_OK);
    TEST_EQ_UINT(idx_map_count(&map), 1u);
    TEST_ASSERT(idx_map_get(&map, idx_slice_make(NULL, 0), NULL));
    TEST_ASSERT(!idx_map_get(&map, key_of("x"), NULL));

    idx_map_free(&map);
}

static void test_iteration(void) {
    idx_map map;
    idx_map_init(&map);

    enum { COUNT = 64 };
    static int values[COUNT];
    char key[32];

    for (int i = 0; i < COUNT; i++) {
        values[i] = i;
        snprintf(key, sizeof(key), "k%d", i);
        TEST_EQ_INT(idx_map_put(&map, key_of(key), &values[i], NULL), IDX_OK);
    }

    int seen[COUNT] = {0};
    size_t cursor = 0;
    idx_slice iter_key;
    void *iter_value = NULL;
    size_t visited = 0;

    while (idx_map_next(&map, &cursor, &iter_key, &iter_value)) {
        int index = *(const int *)iter_value;
        TEST_ASSERT(index >= 0 && index < COUNT);
        seen[index]++;
        visited++;
    }

    TEST_EQ_UINT(visited, COUNT);
    int each_once = 1;
    for (int i = 0; i < COUNT; i++) {
        if (seen[i] != 1) {
            each_once = 0;
        }
    }
    TEST_ASSERT(each_once);

    idx_map_free(&map);
}

static void test_clear_and_reuse(void) {
    idx_map map;
    idx_map_init(&map);

    int value = 1;
    char key[128];
    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof(key), "a-long-enough-key-to-go-on-the-heap-%d", i);
        TEST_EQ_INT(idx_map_put(&map, key_of(key), &value, NULL), IDX_OK);
    }

    size_t capacity = map.capacity;
    idx_map_clear(&map);
    TEST_EQ_UINT(idx_map_count(&map), 0u);
    TEST_EQ_UINT(map.capacity, capacity);
    TEST_ASSERT(!idx_map_get(&map, key_of("a-long-enough-key-to-go-on-the-heap-0"),
                             NULL));

    TEST_EQ_INT(idx_map_put(&map, key_of("after-clear"), &value, NULL), IDX_OK);
    TEST_ASSERT(idx_map_get(&map, key_of("after-clear"), NULL));

    idx_map_free(&map);
    TEST_EQ_UINT(map.capacity, 0u);
}

TEST_MAIN({
    TEST_RUN(test_put_and_get);
    TEST_RUN(test_put_replaces);
    TEST_RUN(test_remove);
    TEST_RUN(test_tombstones_do_not_break_probing);
    TEST_RUN(test_growth);
    TEST_RUN(test_pubkey_keys);
    TEST_RUN(test_keys_are_copied);
    TEST_RUN(test_empty_key);
    TEST_RUN(test_iteration);
    TEST_RUN(test_clear_and_reuse);
})
