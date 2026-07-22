#include "arena.h"

#include <stdint.h>
#include <string.h>

#include "test.h"

static void test_alloc_is_aligned(void) {
    idx_arena arena;
    idx_arena_init(&arena, 1024);

    for (size_t size = 1; size <= 64; size++) {
        void *ptr = NULL;
        TEST_EQ_INT(idx_arena_alloc(&arena, size, &ptr, NULL), IDX_OK);
        TEST_ASSERT(ptr != NULL);
        TEST_EQ_UINT((uintptr_t)ptr % _Alignof(max_align_t), 0u);
        memset(ptr, 0xAB, size); /* the whole range must be writable */
    }

    idx_arena_destroy(&arena);
}

static void test_explicit_alignment(void) {
    idx_arena arena;
    idx_arena_init(&arena, 1024);

    void *unaligned = NULL;
    TEST_EQ_INT(idx_arena_alloc_aligned(&arena, 1, 1, &unaligned, NULL), IDX_OK);

    for (size_t align = 1; align <= 64; align *= 2) {
        void *ptr = NULL;
        TEST_EQ_INT(idx_arena_alloc_aligned(&arena, 8, align, &ptr, NULL),
                    IDX_OK);
        TEST_EQ_UINT((uintptr_t)ptr % align, 0u);
    }

    idx_arena_destroy(&arena);
}

static void test_rejects_bad_alignment(void) {
    idx_arena arena;
    idx_arena_init(&arena, 1024);

    void *ptr = NULL;
    idx_error err;
    idx_error_clear(&err);

    TEST_EQ_INT(idx_arena_alloc_aligned(&arena, 8, 3, &ptr, &err),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(err.status, IDX_ERR_INVALID_ARG);
    TEST_ASSERT(err.message[0] != '\0');

    TEST_EQ_INT(idx_arena_alloc_aligned(&arena, 8, 0, &ptr, NULL),
                IDX_ERR_INVALID_ARG);

    idx_arena_destroy(&arena);
}

/* An allocation larger than the chunk size must still succeed. */
static void test_oversized_allocation(void) {
    idx_arena arena;
    idx_arena_init(&arena, 64);

    void *ptr = NULL;
    TEST_EQ_INT(idx_arena_alloc(&arena, 4096, &ptr, NULL), IDX_OK);
    TEST_ASSERT(ptr != NULL);
    memset(ptr, 0x5A, 4096);
    TEST_ASSERT(arena.bytes_reserved >= 4096);

    idx_arena_destroy(&arena);
}

/* Crossing a chunk boundary must not corrupt previously handed out memory. */
static void test_growth_preserves_data(void) {
    idx_arena arena;
    idx_arena_init(&arena, 128);

    enum { COUNT = 200 };
    unsigned char *blocks[COUNT];

    for (int i = 0; i < COUNT; i++) {
        void *ptr = NULL;
        TEST_EQ_INT(idx_arena_alloc(&arena, 24, &ptr, NULL), IDX_OK);
        blocks[i] = ptr;
        memset(blocks[i], i & 0xFF, 24);
    }

    int intact = 1;
    for (int i = 0; i < COUNT; i++) {
        for (size_t byte = 0; byte < 24; byte++) {
            if (blocks[i][byte] != (unsigned char)(i & 0xFF)) {
                intact = 0;
            }
        }
    }
    TEST_ASSERT(intact);

    idx_arena_destroy(&arena);
}

/* Reset must reuse the existing chunks rather than reserving more. */
static void test_reset_reuses_chunks(void) {
    idx_arena arena;
    idx_arena_init(&arena, 256);

    for (int i = 0; i < 100; i++) {
        void *ptr = NULL;
        TEST_EQ_INT(idx_arena_alloc(&arena, 32, &ptr, NULL), IDX_OK);
    }

    size_t reserved_before = arena.bytes_reserved;
    TEST_ASSERT(reserved_before > 0);

    idx_arena_reset(&arena);
    TEST_EQ_UINT(arena.bytes_used, 0u);
    TEST_EQ_UINT(arena.bytes_reserved, reserved_before);

    for (int i = 0; i < 100; i++) {
        void *ptr = NULL;
        TEST_EQ_INT(idx_arena_alloc(&arena, 32, &ptr, NULL), IDX_OK);
    }
    TEST_EQ_UINT(arena.bytes_reserved, reserved_before);

    idx_arena_destroy(&arena);
}

static void test_calloc_zeroes(void) {
    idx_arena arena;
    idx_arena_init(&arena, 1024);

    void *ptr = NULL;
    TEST_EQ_INT(idx_arena_calloc(&arena, 16, 8, &ptr, NULL), IDX_OK);

    const unsigned char *bytes = ptr;
    int all_zero = 1;
    for (size_t i = 0; i < 16 * 8; i++) {
        if (bytes[i] != 0) {
            all_zero = 0;
        }
    }
    TEST_ASSERT(all_zero);

    /* count * size must be checked for overflow. */
    TEST_EQ_INT(idx_arena_calloc(&arena, SIZE_MAX, 2, &ptr, NULL),
                IDX_ERR_RANGE);

    idx_arena_destroy(&arena);
}

static void test_strdup_copies(void) {
    idx_arena arena;
    idx_arena_init(&arena, 1024);

    const char *original = "11111111111111111111111111111111";
    char *copy = NULL;
    TEST_EQ_INT(idx_arena_strdup(&arena, original, &copy, NULL), IDX_OK);
    TEST_EQ_STR(copy, original);
    TEST_ASSERT(copy != original);

    idx_arena_destroy(&arena);
}

/* Destroy must leave the arena usable again. */
static void test_destroy_is_reusable(void) {
    idx_arena arena;
    idx_arena_init(&arena, 512);

    void *ptr = NULL;
    TEST_EQ_INT(idx_arena_alloc(&arena, 100, &ptr, NULL), IDX_OK);
    idx_arena_destroy(&arena);

    TEST_EQ_UINT(arena.bytes_reserved, 0u);
    TEST_EQ_UINT(arena.chunk_size, 512u);
    TEST_EQ_INT(idx_arena_alloc(&arena, 100, &ptr, NULL), IDX_OK);

    idx_arena_destroy(&arena);
}

TEST_MAIN({
    TEST_RUN(test_alloc_is_aligned);
    TEST_RUN(test_explicit_alignment);
    TEST_RUN(test_rejects_bad_alignment);
    TEST_RUN(test_oversized_allocation);
    TEST_RUN(test_growth_preserves_data);
    TEST_RUN(test_reset_reuses_chunks);
    TEST_RUN(test_calloc_zeroes);
    TEST_RUN(test_strdup_copies);
    TEST_RUN(test_destroy_is_reusable);
})
