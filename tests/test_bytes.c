#include "bytes.h"

#include <string.h>

#include "test.h"

static void test_slice_basics(void) {
    const uint8_t raw[] = {1, 2, 3, 4};
    idx_slice slice = idx_slice_make(raw, sizeof(raw));

    TEST_EQ_UINT(slice.len, 4u);
    TEST_ASSERT(!idx_slice_is_empty(slice));
    TEST_ASSERT(idx_slice_is_empty(idx_slice_make(NULL, 0)));

    /* A NULL pointer yields an empty slice regardless of the length given. */
    TEST_EQ_UINT(idx_slice_make(NULL, 9).len, 0u);

    idx_slice from_str = idx_slice_from_str("abc");
    TEST_EQ_UINT(from_str.len, 3u); /* the terminator is not included */
    TEST_EQ_UINT(idx_slice_from_str(NULL).len, 0u);
}

static void test_slice_equal(void) {
    idx_slice a = idx_slice_from_str("hello");
    idx_slice b = idx_slice_from_str("hello");
    idx_slice c = idx_slice_from_str("hellp");
    idx_slice d = idx_slice_from_str("hell");

    TEST_ASSERT(idx_slice_equal(a, b));
    TEST_ASSERT(!idx_slice_equal(a, c));
    TEST_ASSERT(!idx_slice_equal(a, d));
    TEST_ASSERT(idx_slice_equal(idx_slice_make(NULL, 0), idx_slice_make(NULL, 0)));
}

static void test_slice_sub(void) {
    idx_slice slice = idx_slice_from_str("abcdef");

    TEST_ASSERT(idx_slice_equal(idx_slice_sub(slice, 0, 3),
                                idx_slice_from_str("abc")));
    TEST_ASSERT(idx_slice_equal(idx_slice_sub(slice, 3, 3),
                                idx_slice_from_str("def")));

    /* The length is clamped rather than read past the end. */
    TEST_ASSERT(idx_slice_equal(idx_slice_sub(slice, 4, 100),
                                idx_slice_from_str("ef")));

    TEST_ASSERT(idx_slice_is_empty(idx_slice_sub(slice, 6, 1)));
    TEST_ASSERT(idx_slice_is_empty(idx_slice_sub(slice, 99, 1)));
}

static void test_cursor_reads(void) {
    const uint8_t raw[] = {
        0xAB,                                           /* u8 */
        0x34, 0x12,                                     /* u16 le */
        0x78, 0x56, 0x34, 0x12,                         /* u32 le */
        0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12, /* u64 le */
    };

    idx_cursor cursor;
    idx_cursor_init(&cursor, idx_slice_make(raw, sizeof(raw)));
    TEST_EQ_UINT(idx_cursor_remaining(&cursor), sizeof(raw));

    uint8_t u8 = 0;
    uint16_t u16 = 0;
    uint32_t u32 = 0;
    uint64_t u64 = 0;

    TEST_EQ_INT(idx_cursor_u8(&cursor, &u8, NULL), IDX_OK);
    TEST_EQ_UINT(u8, 0xABu);
    TEST_EQ_INT(idx_cursor_u16le(&cursor, &u16, NULL), IDX_OK);
    TEST_EQ_UINT(u16, 0x1234u);
    TEST_EQ_INT(idx_cursor_u32le(&cursor, &u32, NULL), IDX_OK);
    TEST_EQ_UINT(u32, 0x12345678u);
    TEST_EQ_INT(idx_cursor_u64le(&cursor, &u64, NULL), IDX_OK);
    TEST_EQ_UINT(u64, 0x123456789ABCDEF0ull);

    TEST_ASSERT(idx_cursor_exhausted(&cursor));
    TEST_EQ_UINT(idx_cursor_remaining(&cursor), 0u);
}

static void test_cursor_bounds(void) {
    const uint8_t raw[] = {1, 2, 3};
    idx_cursor cursor;
    idx_cursor_init(&cursor, idx_slice_make(raw, sizeof(raw)));

    idx_error err;
    idx_error_clear(&err);

    idx_slice taken;
    TEST_EQ_INT(idx_cursor_take(&cursor, 4, &taken, &err), IDX_ERR_RANGE);
    TEST_ASSERT(strstr(err.message, "only 3 remain") != NULL);

    /* A failed read must not advance the cursor. */
    TEST_EQ_UINT(cursor.offset, 0u);

    TEST_EQ_INT(idx_cursor_take(&cursor, 2, &taken, NULL), IDX_OK);
    TEST_EQ_UINT(taken.len, 2u);
    TEST_EQ_UINT(cursor.offset, 2u);

    uint32_t u32 = 0;
    TEST_EQ_INT(idx_cursor_u32le(&cursor, &u32, NULL), IDX_ERR_RANGE);
    TEST_EQ_UINT(cursor.offset, 2u);

    TEST_EQ_INT(idx_cursor_skip(&cursor, 1, NULL), IDX_OK);
    TEST_ASSERT(idx_cursor_exhausted(&cursor));

    /* A zero-length read at the end is legal. */
    TEST_EQ_INT(idx_cursor_take(&cursor, 0, &taken, NULL), IDX_OK);
}

static void test_cursor_copy(void) {
    const uint8_t raw[] = {9, 8, 7, 6};
    idx_cursor cursor;
    idx_cursor_init(&cursor, idx_slice_make(raw, sizeof(raw)));

    uint8_t dest[4] = {0};
    TEST_EQ_INT(idx_cursor_copy(&cursor, dest, 4, NULL), IDX_OK);
    TEST_EQ_INT(memcmp(dest, raw, 4), 0);
}

static void test_buffer_append(void) {
    idx_buffer buffer;
    idx_buffer_init(&buffer);

    TEST_EQ_UINT(buffer.len, 0u);
    TEST_ASSERT(buffer.data == NULL);

    TEST_EQ_INT(idx_buffer_append(&buffer, "abc", 3, NULL), IDX_OK);
    TEST_EQ_INT(idx_buffer_append_byte(&buffer, 'd', NULL), IDX_OK);
    TEST_EQ_UINT(buffer.len, 4u);
    TEST_ASSERT(idx_slice_equal(idx_buffer_slice(&buffer),
                                idx_slice_from_str("abcd")));

    /* Appending nothing is a no-op, even with a NULL pointer. */
    TEST_EQ_INT(idx_buffer_append(&buffer, NULL, 0, NULL), IDX_OK);
    TEST_EQ_UINT(buffer.len, 4u);

    /* A NULL pointer with a non-zero length is a caller bug. */
    TEST_EQ_INT(idx_buffer_append(&buffer, NULL, 1, NULL), IDX_ERR_INVALID_ARG);

    idx_buffer_free(&buffer);
    TEST_ASSERT(buffer.data == NULL);
    TEST_EQ_UINT(buffer.capacity, 0u);
}

/* Growth must preserve contents and clear must keep the allocation. */
static void test_buffer_growth_and_clear(void) {
    idx_buffer buffer;
    idx_buffer_init(&buffer);

    for (size_t i = 0; i < 5000; i++) {
        uint8_t byte = (uint8_t)(i & 0xFF);
        TEST_EQ_INT(idx_buffer_append_byte(&buffer, byte, NULL), IDX_OK);
    }
    TEST_EQ_UINT(buffer.len, 5000u);

    int intact = 1;
    for (size_t i = 0; i < 5000; i++) {
        if (buffer.data[i] != (uint8_t)(i & 0xFF)) {
            intact = 0;
        }
    }
    TEST_ASSERT(intact);

    size_t capacity = buffer.capacity;
    idx_buffer_clear(&buffer);
    TEST_EQ_UINT(buffer.len, 0u);
    TEST_EQ_UINT(buffer.capacity, capacity);

    idx_buffer_free(&buffer);
}

static void test_hex_encode(void) {
    const uint8_t raw[] = {0x00, 0x0F, 0xA5, 0xFF};
    char out[16];

    TEST_EQ_INT(idx_hex_encode(idx_slice_make(raw, sizeof(raw)), out,
                               sizeof(out), NULL),
                IDX_OK);
    TEST_EQ_STR(out, "000fa5ff");

    TEST_EQ_UINT(idx_hex_encoded_size(4), 8u);

    /* One byte short of the terminator. */
    char tight[8];
    TEST_EQ_INT(idx_hex_encode(idx_slice_make(raw, sizeof(raw)), tight,
                               sizeof(tight), NULL),
                IDX_ERR_RANGE);

    char empty[2];
    TEST_EQ_INT(idx_hex_encode(idx_slice_make(NULL, 0), empty, sizeof(empty),
                               NULL),
                IDX_OK);
    TEST_EQ_STR(empty, "");
}

static void test_hex_decode(void) {
    uint8_t out[8];
    size_t out_len = 0;

    TEST_EQ_INT(idx_hex_decode("000FA5ff", 8, out, sizeof(out), &out_len, NULL),
                IDX_OK);
    TEST_EQ_UINT(out_len, 4u);
    TEST_EQ_UINT(out[0], 0x00u);
    TEST_EQ_UINT(out[1], 0x0Fu);
    TEST_EQ_UINT(out[2], 0xA5u);
    TEST_EQ_UINT(out[3], 0xFFu);

    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_hex_decode("abc", 3, out, sizeof(out), NULL, &err),
                IDX_ERR_PARSE);
    TEST_ASSERT(strstr(err.message, "odd length") != NULL);

    TEST_EQ_INT(idx_hex_decode("zz", 2, out, sizeof(out), NULL, NULL),
                IDX_ERR_PARSE);
    TEST_EQ_INT(idx_hex_decode("0011", 4, out, 1, NULL, NULL), IDX_ERR_RANGE);
}

TEST_MAIN({
    TEST_RUN(test_slice_basics);
    TEST_RUN(test_slice_equal);
    TEST_RUN(test_slice_sub);
    TEST_RUN(test_cursor_reads);
    TEST_RUN(test_cursor_bounds);
    TEST_RUN(test_cursor_copy);
    TEST_RUN(test_buffer_append);
    TEST_RUN(test_buffer_growth_and_clear);
    TEST_RUN(test_hex_encode);
    TEST_RUN(test_hex_decode);
})
