#include "base58.h"

#include <string.h>

#include "bytes.h"
#include "test.h"

/*
 * Vectors produced by an independent Python implementation, each verified to
 * round-trip there before being pasted here.
 */
static const struct {
    const char *encoded;
    const char *hex;
    const char *what;
} k_vectors[] = {
    {"", "", "empty"},
    {"1", "00", "single zero byte"},
    {"111", "000000", "three zero bytes"},
    {"2g", "61", "0x61"},
    {"a3gV", "626262", "0x626262"},
    {"aPEr", "636363", "0x636363"},
    {"12", "0001", "leading zero then one"},
    {"5Q", "ff", "0xff"},
    {"LUv", "ffff", "0xffff"},
    {"JEKNVnkbo3jma5nREBBJCDoXFVeKkD56V3xKrvRmWxFG",
     "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "32 bytes of 0xff"},
    {"11111111111111111111111111111111",
     "0000000000000000000000000000000000000000000000000000000000000000",
     "32 zero bytes"},
    {"1thX6LZfHDZZKUs92febYZhYRcXddmzfzF2NvTkPNE",
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
     "incrementing 32 bytes"},
};

static const size_t k_vector_count = sizeof(k_vectors) / sizeof(k_vectors[0]);

static void test_encode_vectors(void) {
    for (size_t i = 0; i < k_vector_count; i++) {
        uint8_t raw[64];
        size_t raw_len = 0;
        TEST_EQ_INT(idx_hex_decode(k_vectors[i].hex, strlen(k_vectors[i].hex),
                                   raw, sizeof(raw), &raw_len, NULL),
                    IDX_OK);

        char encoded[128];
        size_t encoded_len = 0;
        idx_error err;
        idx_error_clear(&err);
        TEST_EQ_INT(idx_base58_encode(idx_slice_make(raw, raw_len), encoded,
                                      sizeof(encoded), &encoded_len, &err),
                    IDX_OK);

        TEST_CHECK(strcmp(encoded, k_vectors[i].encoded) == 0,
                   "%s: expected \"%s\", got \"%s\"", k_vectors[i].what,
                   k_vectors[i].encoded, encoded);
        TEST_EQ_UINT(encoded_len, strlen(k_vectors[i].encoded));
    }
}

static void test_decode_vectors(void) {
    for (size_t i = 0; i < k_vector_count; i++) {
        uint8_t expected[64];
        size_t expected_len = 0;
        TEST_EQ_INT(idx_hex_decode(k_vectors[i].hex, strlen(k_vectors[i].hex),
                                   expected, sizeof(expected), &expected_len,
                                   NULL),
                    IDX_OK);

        uint8_t decoded[64];
        size_t decoded_len = 0;
        TEST_EQ_INT(idx_base58_decode(k_vectors[i].encoded,
                                      strlen(k_vectors[i].encoded), decoded,
                                      sizeof(decoded), &decoded_len, NULL),
                    IDX_OK);

        TEST_CHECK(decoded_len == expected_len, "%s: expected %zu bytes, got %zu",
                   k_vectors[i].what, expected_len, decoded_len);
        if (decoded_len == expected_len) {
            TEST_CHECK(memcmp(decoded, expected, expected_len) == 0,
                       "%s: decoded bytes differ", k_vectors[i].what);
        }
    }
}

/* Every byte pattern must survive a round trip, including leading zeros. */
static void test_round_trip(void) {
    for (size_t len = 0; len <= 64; len++) {
        uint8_t raw[64];
        for (size_t i = 0; i < len; i++) {
            raw[i] = (uint8_t)((i * 37 + len) & 0xFF);
        }
        /* Force leading zeros on some iterations. */
        if (len > 3 && len % 3 == 0) {
            raw[0] = 0;
            raw[1] = 0;
        }

        char encoded[128];
        TEST_EQ_INT(idx_base58_encode(idx_slice_make(raw, len), encoded,
                                      sizeof(encoded), NULL, NULL),
                    IDX_OK);

        uint8_t decoded[64];
        size_t decoded_len = 0;
        TEST_EQ_INT(idx_base58_decode(encoded, strlen(encoded), decoded,
                                      sizeof(decoded), &decoded_len, NULL),
                    IDX_OK);

        TEST_CHECK(decoded_len == len, "length %zu round-tripped as %zu", len,
                   decoded_len);
        if (decoded_len == len) {
            TEST_CHECK(memcmp(decoded, raw, len) == 0,
                       "length %zu round-tripped to different bytes", len);
        }
    }
}

/* Inputs larger than the stack scratch must take the heap path correctly. */
static void test_large_input(void) {
    uint8_t raw[1024];
    for (size_t i = 0; i < sizeof(raw); i++) {
        raw[i] = (uint8_t)(i & 0xFF);
    }

    char encoded[2048];
    TEST_EQ_INT(idx_base58_encode(idx_slice_make(raw, sizeof(raw)), encoded,
                                  sizeof(encoded), NULL, NULL),
                IDX_OK);

    uint8_t decoded[1024];
    size_t decoded_len = 0;
    TEST_EQ_INT(idx_base58_decode(encoded, strlen(encoded), decoded,
                                  sizeof(decoded), &decoded_len, NULL),
                IDX_OK);
    TEST_EQ_UINT(decoded_len, sizeof(raw));
    TEST_EQ_INT(memcmp(decoded, raw, sizeof(raw)), 0);
}

static void test_rejects_invalid_characters(void) {
    uint8_t out[64];
    idx_error err;

    /* 0, O, I and l are deliberately absent from the alphabet. */
    const char *invalid[] = {"0", "O", "I", "l", "abc0def", "hello world", "+"};

    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        idx_error_clear(&err);
        TEST_CHECK(idx_base58_decode(invalid[i], strlen(invalid[i]), out,
                                     sizeof(out), NULL, &err) == IDX_ERR_PARSE,
                   "\"%s\" should have been rejected", invalid[i]);
    }

    idx_error_clear(&err);
    idx_base58_decode("ab0cd", 5, out, sizeof(out), NULL, &err);
    TEST_ASSERT(strstr(err.message, "offset 2") != NULL);
}

static void test_output_too_small(void) {
    const uint8_t raw[32] = {1};
    char tiny[4];
    TEST_EQ_INT(idx_base58_encode(idx_slice_make(raw, sizeof(raw)), tiny,
                                  sizeof(tiny), NULL, NULL),
                IDX_ERR_RANGE);

    uint8_t out[2];
    TEST_EQ_INT(idx_base58_decode("11111111111111111111111111111111", 32, out,
                                  sizeof(out), NULL, NULL),
                IDX_ERR_RANGE);
}

static void test_decode_exact(void) {
    uint8_t key[32];
    const char *system_program = "11111111111111111111111111111111";

    TEST_EQ_INT(idx_base58_decode_exact(system_program, 32, key, 32, NULL),
                IDX_OK);
    for (size_t i = 0; i < 32; i++) {
        TEST_EQ_UINT(key[i], 0u);
    }

    /* A string that decodes to the wrong length is a parse error. */
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_base58_decode_exact("2g", 2, key, 32, &err), IDX_ERR_PARSE);
}

static void test_size_bounds(void) {
    /* The bounds must never be under the real requirement. */
    for (size_t len = 0; len <= 128; len++) {
        uint8_t raw[128];
        memset(raw, 0xFF, len);

        char encoded[256];
        size_t encoded_len = 0;
        TEST_EQ_INT(idx_base58_encode(idx_slice_make(raw, len), encoded,
                                      sizeof(encoded), &encoded_len, NULL),
                    IDX_OK);
        TEST_CHECK(encoded_len <= idx_base58_encoded_max(len),
                   "encoded %zu bytes into %zu chars, bound said %zu", len,
                   encoded_len, idx_base58_encoded_max(len));

        uint8_t decoded[256];
        size_t decoded_len = 0;
        TEST_EQ_INT(idx_base58_decode(encoded, encoded_len, decoded,
                                      sizeof(decoded), &decoded_len, NULL),
                    IDX_OK);
        TEST_CHECK(decoded_len <= idx_base58_decoded_max(encoded_len),
                   "decoded %zu chars into %zu bytes, bound said %zu",
                   encoded_len, decoded_len,
                   idx_base58_decoded_max(encoded_len));
    }
}

TEST_MAIN({
    TEST_RUN(test_encode_vectors);
    TEST_RUN(test_decode_vectors);
    TEST_RUN(test_round_trip);
    TEST_RUN(test_large_input);
    TEST_RUN(test_rejects_invalid_characters);
    TEST_RUN(test_output_too_small);
    TEST_RUN(test_decode_exact);
    TEST_RUN(test_size_bounds);
})
