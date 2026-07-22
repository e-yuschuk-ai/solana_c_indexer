#include "base64.h"

#include <string.h>

#include "bytes.h"
#include "test.h"

/* RFC 4648 section 10 test vectors. */
static const struct {
    const char *plain;
    const char *encoded;
} k_rfc_vectors[] = {
    {"", ""},           {"f", "Zg=="},         {"fo", "Zm8="},
    {"foo", "Zm9v"},    {"foob", "Zm9vYg=="},  {"fooba", "Zm9vYmE="},
    {"foobar", "Zm9vYmFy"},
};

/* Binary cases, including the two characters that differ between variants. */
static const struct {
    const char *hex;
    const char *encoded;
} k_binary_vectors[] = {
    {"00", "AA=="},
    {"ffef", "/+8="},
    {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
     "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="},
};

static void test_encode_rfc_vectors(void) {
    for (size_t i = 0; i < sizeof(k_rfc_vectors) / sizeof(k_rfc_vectors[0]);
         i++) {
        char out[64];
        size_t out_len = 0;
        TEST_EQ_INT(idx_base64_encode(idx_slice_from_str(k_rfc_vectors[i].plain),
                                      out, sizeof(out), &out_len, NULL),
                    IDX_OK);
        TEST_CHECK(strcmp(out, k_rfc_vectors[i].encoded) == 0,
                   "\"%s\": expected \"%s\", got \"%s\"", k_rfc_vectors[i].plain,
                   k_rfc_vectors[i].encoded, out);
        TEST_EQ_UINT(out_len, strlen(k_rfc_vectors[i].encoded));
        TEST_EQ_UINT(idx_base64_encoded_size(strlen(k_rfc_vectors[i].plain)),
                     strlen(k_rfc_vectors[i].encoded));
    }
}

static void test_decode_rfc_vectors(void) {
    for (size_t i = 0; i < sizeof(k_rfc_vectors) / sizeof(k_rfc_vectors[0]);
         i++) {
        uint8_t out[64];
        size_t out_len = 0;
        TEST_EQ_INT(idx_base64_decode(k_rfc_vectors[i].encoded,
                                      strlen(k_rfc_vectors[i].encoded), out,
                                      sizeof(out), &out_len, NULL),
                    IDX_OK);
        TEST_EQ_UINT(out_len, strlen(k_rfc_vectors[i].plain));
        TEST_CHECK(memcmp(out, k_rfc_vectors[i].plain, out_len) == 0,
                   "\"%s\" did not decode back", k_rfc_vectors[i].encoded);
    }
}

static void test_binary_vectors(void) {
    for (size_t i = 0;
         i < sizeof(k_binary_vectors) / sizeof(k_binary_vectors[0]); i++) {
        uint8_t raw[64];
        size_t raw_len = 0;
        TEST_EQ_INT(idx_hex_decode(k_binary_vectors[i].hex,
                                   strlen(k_binary_vectors[i].hex), raw,
                                   sizeof(raw), &raw_len, NULL),
                    IDX_OK);

        char encoded[128];
        TEST_EQ_INT(idx_base64_encode(idx_slice_make(raw, raw_len), encoded,
                                      sizeof(encoded), NULL, NULL),
                    IDX_OK);
        TEST_CHECK(strcmp(encoded, k_binary_vectors[i].encoded) == 0,
                   "%s: expected \"%s\", got \"%s\"", k_binary_vectors[i].hex,
                   k_binary_vectors[i].encoded, encoded);

        uint8_t decoded[64];
        size_t decoded_len = 0;
        TEST_EQ_INT(idx_base64_decode(encoded, strlen(encoded), decoded,
                                      sizeof(decoded), &decoded_len, NULL),
                    IDX_OK);
        TEST_EQ_UINT(decoded_len, raw_len);
        TEST_EQ_INT(memcmp(decoded, raw, raw_len), 0);
    }
}

static void test_round_trip(void) {
    for (size_t len = 0; len <= 256; len++) {
        uint8_t raw[256];
        for (size_t i = 0; i < len; i++) {
            raw[i] = (uint8_t)((i * 61 + len) & 0xFF);
        }

        char encoded[512];
        size_t encoded_len = 0;
        TEST_EQ_INT(idx_base64_encode(idx_slice_make(raw, len), encoded,
                                      sizeof(encoded), &encoded_len, NULL),
                    IDX_OK);
        TEST_CHECK(encoded_len % 4 == 0, "length %zu encoded to %zu chars", len,
                   encoded_len);

        uint8_t decoded[256];
        size_t decoded_len = 0;
        TEST_EQ_INT(idx_base64_decode(encoded, encoded_len, decoded,
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

static void test_rejects_malformed(void) {
    uint8_t out[64];
    idx_error err;

    /* Not a multiple of four. */
    idx_error_clear(&err);
    TEST_EQ_INT(idx_base64_decode("Zm9", 3, out, sizeof(out), NULL, &err),
                IDX_ERR_PARSE);
    TEST_ASSERT(strstr(err.message, "multiple of four") != NULL);

    /* Characters outside the alphabet, including whitespace. */
    const char *invalid[] = {"Zm9v Zm9v", "Zm9v\n\n\n\n", "****", "Zm-v"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        TEST_CHECK(idx_base64_decode(invalid[i], strlen(invalid[i]), out,
                                     sizeof(out), NULL, NULL) == IDX_ERR_PARSE,
                   "\"%s\" should have been rejected", invalid[i]);
    }

    /* Padding in the wrong place. */
    const char *bad_padding[] = {"=AAA", "A=AA", "AA=A", "Zm==Zm9v"};
    for (size_t i = 0; i < sizeof(bad_padding) / sizeof(bad_padding[0]); i++) {
        TEST_CHECK(idx_base64_decode(bad_padding[i], strlen(bad_padding[i]), out,
                                     sizeof(out), NULL, NULL) == IDX_ERR_PARSE,
                   "\"%s\" should have been rejected", bad_padding[i]);
    }
}

static void test_output_too_small(void) {
    char small[4];
    TEST_EQ_INT(idx_base64_encode(idx_slice_from_str("foobar"), small,
                                  sizeof(small), NULL, NULL),
                IDX_ERR_RANGE);

    uint8_t out[2];
    TEST_EQ_INT(idx_base64_decode("Zm9vYmFy", 8, out, sizeof(out), NULL, NULL),
                IDX_ERR_RANGE);
}

TEST_MAIN({
    TEST_RUN(test_encode_rfc_vectors);
    TEST_RUN(test_decode_rfc_vectors);
    TEST_RUN(test_binary_vectors);
    TEST_RUN(test_round_trip);
    TEST_RUN(test_rejects_malformed);
    TEST_RUN(test_output_too_small);
})
