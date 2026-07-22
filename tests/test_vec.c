#include "vec.h"

#include <stdint.h>
#include <string.h>

#include "test.h"

static void test_push_and_index(void) {
    idx_vec vec;
    idx_vec_init(&vec, sizeof(uint32_t));

    TEST_ASSERT(idx_vec_is_empty(&vec));
    TEST_ASSERT(vec.data == NULL);

    for (uint32_t i = 0; i < 100; i++) {
        TEST_EQ_INT(idx_vec_push(&vec, &i, NULL), IDX_OK);
    }
    TEST_EQ_UINT(idx_vec_len(&vec), 100u);

    int intact = 1;
    for (uint32_t i = 0; i < 100; i++) {
        const uint32_t *value = idx_vec_at(&vec, i);
        if (value == NULL || *value != i) {
            intact = 0;
        }
    }
    TEST_ASSERT(intact);

    TEST_ASSERT(idx_vec_at(&vec, 100) == NULL);
    TEST_ASSERT(idx_vec_at(&vec, SIZE_MAX) == NULL);

    idx_vec_free(&vec);
}

static void test_push_uninit(void) {
    typedef struct {
        uint64_t slot;
        uint8_t flag;
    } record;

    idx_vec vec;
    idx_vec_init(&vec, sizeof(record));

    record *slot = NULL;
    TEST_EQ_INT(idx_vec_push_uninit(&vec, (void **)&slot, NULL), IDX_OK);
    slot->slot = 42;
    slot->flag = 1;

    const record *stored = idx_vec_at(&vec, 0);
    TEST_EQ_UINT(stored->slot, 42u);
    TEST_EQ_UINT(stored->flag, 1u);
    TEST_EQ_UINT(idx_vec_len(&vec), 1u);

    idx_vec_free(&vec);
}

static void test_pop(void) {
    idx_vec vec;
    idx_vec_init(&vec, sizeof(int));

    for (int i = 0; i < 3; i++) {
        TEST_EQ_INT(idx_vec_push(&vec, &i, NULL), IDX_OK);
    }

    int value = 0;
    TEST_ASSERT(idx_vec_pop(&vec, &value));
    TEST_EQ_INT(value, 2);
    TEST_EQ_UINT(idx_vec_len(&vec), 2u);

    /* Discarding the popped value is allowed. */
    TEST_ASSERT(idx_vec_pop(&vec, NULL));
    TEST_ASSERT(idx_vec_pop(&vec, &value));
    TEST_EQ_INT(value, 0);

    TEST_ASSERT(!idx_vec_pop(&vec, &value));
    TEST_ASSERT(idx_vec_is_empty(&vec));

    idx_vec_free(&vec);
}

/* Growth must preserve every element across many reallocations. */
static void test_growth_preserves_elements(void) {
    idx_vec vec;
    idx_vec_init(&vec, sizeof(uint64_t));

    enum { COUNT = 10000 };
    for (uint64_t i = 0; i < COUNT; i++) {
        uint64_t value = i * 2654435761ull;
        TEST_EQ_INT(idx_vec_push(&vec, &value, NULL), IDX_OK);
    }

    int intact = 1;
    for (uint64_t i = 0; i < COUNT; i++) {
        const uint64_t *value = idx_vec_at(&vec, i);
        if (value == NULL || *value != i * 2654435761ull) {
            intact = 0;
        }
    }
    TEST_ASSERT(intact);
    TEST_ASSERT(vec.capacity >= COUNT);

    idx_vec_free(&vec);
}

static void test_reserve(void) {
    idx_vec vec;
    idx_vec_init(&vec, sizeof(int));

    TEST_EQ_INT(idx_vec_reserve(&vec, 1000, NULL), IDX_OK);
    TEST_ASSERT(vec.capacity >= 1000);
    TEST_EQ_UINT(idx_vec_len(&vec), 0u);

    const void *before = vec.data;
    for (int i = 0; i < 1000; i++) {
        TEST_EQ_INT(idx_vec_push(&vec, &i, NULL), IDX_OK);
    }
    /* Reserved capacity means no reallocation happened. */
    TEST_ASSERT(vec.data == before);

    idx_vec_free(&vec);
}

static void test_clear_keeps_capacity(void) {
    idx_vec vec;
    idx_vec_init(&vec, sizeof(int));

    for (int i = 0; i < 50; i++) {
        TEST_EQ_INT(idx_vec_push(&vec, &i, NULL), IDX_OK);
    }
    size_t capacity = vec.capacity;

    idx_vec_clear(&vec);
    TEST_EQ_UINT(idx_vec_len(&vec), 0u);
    TEST_EQ_UINT(vec.capacity, capacity);

    idx_vec_free(&vec);
    TEST_EQ_UINT(vec.capacity, 0u);
    TEST_EQ_UINT(vec.elem_size, sizeof(int)); /* still usable */

    int value = 7;
    TEST_EQ_INT(idx_vec_push(&vec, &value, NULL), IDX_OK);
    idx_vec_free(&vec);
}

static void test_rejects_invalid(void) {
    idx_vec vec;
    idx_vec_init(&vec, 0);

    int value = 1;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_vec_push(&vec, &value, &err), IDX_ERR_INVALID_ARG);
    TEST_ASSERT(strstr(err.message, "elem_size") != NULL);

    idx_vec_init(&vec, sizeof(int));
    TEST_EQ_INT(idx_vec_push(&vec, NULL, NULL), IDX_ERR_INVALID_ARG);

    /* An allocation this large must be refused, not attempted. */
    TEST_EQ_INT(idx_vec_reserve(&vec, SIZE_MAX, NULL), IDX_ERR_RANGE);

    idx_vec_free(&vec);
}

TEST_MAIN({
    TEST_RUN(test_push_and_index);
    TEST_RUN(test_push_uninit);
    TEST_RUN(test_pop);
    TEST_RUN(test_growth_preserves_elements);
    TEST_RUN(test_reserve);
    TEST_RUN(test_clear_keeps_capacity);
    TEST_RUN(test_rejects_invalid);
})
