#include "types.h"

#include <string.h>

#include "test.h"

/*
 * The program ids are stored as raw bytes in types.c. These assertions are
 * what keeps those literals honest: if a byte is wrong, the base58 form will
 * not match the id everyone knows.
 */
static void test_well_known_program_ids(void) {
    const struct {
        const idx_pubkey *key;
        const char *expected;
        const char *name;
    } programs[] = {
        {&IDX_PROGRAM_SYSTEM, "11111111111111111111111111111111", "system"},
        {&IDX_PROGRAM_TOKEN, "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
         "token"},
        {&IDX_PROGRAM_TOKEN_2022, "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb",
         "token-2022"},
        {&IDX_PROGRAM_VOTE, "Vote111111111111111111111111111111111111111",
         "vote"},
        {&IDX_PROGRAM_MEMO, "MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr",
         "memo"},
    };

    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
        char text[IDX_PUBKEY_STR_MAX];
        TEST_EQ_INT(idx_pubkey_to_base58(programs[i].key, text, NULL), IDX_OK);
        TEST_CHECK(strcmp(text, programs[i].expected) == 0,
                   "%s: expected \"%s\", got \"%s\"", programs[i].name,
                   programs[i].expected, text);

        /* And the text must decode back to the same bytes. */
        idx_pubkey parsed;
        TEST_EQ_INT(idx_pubkey_from_base58(programs[i].expected,
                                           strlen(programs[i].expected),
                                           &parsed, NULL),
                    IDX_OK);
        TEST_CHECK(idx_pubkey_equal(&parsed, programs[i].key),
                   "%s did not round-trip", programs[i].name);
    }
}

static void test_pubkey_default(void) {
    TEST_ASSERT(idx_pubkey_is_default(&IDX_PUBKEY_DEFAULT));
    TEST_ASSERT(idx_pubkey_equal(&IDX_PUBKEY_DEFAULT, &IDX_PROGRAM_SYSTEM));
    TEST_ASSERT(!idx_pubkey_is_default(&IDX_PROGRAM_TOKEN));

    idx_slice slice = idx_pubkey_slice(&IDX_PROGRAM_TOKEN);
    TEST_EQ_UINT(slice.len, (unsigned)IDX_PUBKEY_LEN);
    TEST_ASSERT(slice.data == IDX_PROGRAM_TOKEN.bytes);
}

static void test_pubkey_from_bytes(void) {
    uint8_t raw[IDX_PUBKEY_LEN];
    for (size_t i = 0; i < sizeof(raw); i++) {
        raw[i] = (uint8_t)i;
    }

    idx_pubkey key = idx_pubkey_from_bytes(raw);
    TEST_EQ_INT(memcmp(key.bytes, raw, sizeof(raw)), 0);

    char text[IDX_PUBKEY_STR_MAX];
    TEST_EQ_INT(idx_pubkey_to_base58(&key, text, NULL), IDX_OK);
    TEST_EQ_STR(text, "1thX6LZfHDZZKUs92febYZhYRcXddmzfzF2NvTkPNE");
}

/* A pubkey-length string must not be accepted as a signature, or vice versa. */
static void test_length_is_enforced(void) {
    const char *pubkey_text = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";

    idx_signature signature;
    TEST_EQ_INT(idx_signature_from_base58(pubkey_text, strlen(pubkey_text),
                                          &signature, NULL),
                IDX_ERR_PARSE);

    idx_pubkey key;
    TEST_EQ_INT(idx_pubkey_from_base58("2g", 2, &key, NULL), IDX_ERR_PARSE);
}

static void test_signature_round_trip(void) {
    idx_signature signature;
    for (size_t i = 0; i < IDX_SIGNATURE_LEN; i++) {
        signature.bytes[i] = (uint8_t)((i * 7 + 3) & 0xFF);
    }

    char text[IDX_SIGNATURE_STR_MAX];
    TEST_EQ_INT(idx_signature_to_base58(&signature, text, NULL), IDX_OK);

    idx_signature parsed;
    TEST_EQ_INT(idx_signature_from_base58(text, strlen(text), &parsed, NULL),
                IDX_OK);
    TEST_ASSERT(idx_signature_equal(&signature, &parsed));

    signature.bytes[0] ^= 0xFF;
    TEST_ASSERT(!idx_signature_equal(&signature, &parsed));
}

static void test_hash_round_trip(void) {
    idx_hash hash;
    for (size_t i = 0; i < IDX_HASH_LEN; i++) {
        hash.bytes[i] = (uint8_t)(255 - i);
    }

    char text[IDX_HASH_STR_MAX];
    TEST_EQ_INT(idx_hash_to_base58(&hash, text, NULL), IDX_OK);

    idx_hash parsed;
    TEST_EQ_INT(idx_hash_from_base58(text, strlen(text), &parsed, NULL), IDX_OK);
    TEST_ASSERT(idx_hash_equal(&hash, &parsed));
}

/*
 * The declared buffer sizes must hold the longest possible encoding, which is
 * the all-0xff value for each type.
 */
static void test_string_buffers_are_large_enough(void) {
    idx_pubkey key;
    memset(key.bytes, 0xFF, sizeof(key.bytes));
    char key_text[IDX_PUBKEY_STR_MAX];
    TEST_EQ_INT(idx_pubkey_to_base58(&key, key_text, NULL), IDX_OK);
    TEST_ASSERT(strlen(key_text) < IDX_PUBKEY_STR_MAX);

    idx_signature signature;
    memset(signature.bytes, 0xFF, sizeof(signature.bytes));
    char signature_text[IDX_SIGNATURE_STR_MAX];
    TEST_EQ_INT(idx_signature_to_base58(&signature, signature_text, NULL),
                IDX_OK);
    TEST_ASSERT(strlen(signature_text) < IDX_SIGNATURE_STR_MAX);

    idx_hash hash;
    memset(hash.bytes, 0xFF, sizeof(hash.bytes));
    char hash_text[IDX_HASH_STR_MAX];
    TEST_EQ_INT(idx_hash_to_base58(&hash, hash_text, NULL), IDX_OK);
    TEST_ASSERT(strlen(hash_text) < IDX_HASH_STR_MAX);
}

static void test_rejects_null(void) {
    idx_pubkey key;
    TEST_EQ_INT(idx_pubkey_from_base58("11111111111111111111111111111111", 32,
                                       NULL, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_pubkey_to_base58(&key, NULL, NULL), IDX_ERR_INVALID_ARG);
}

TEST_MAIN({
    TEST_RUN(test_well_known_program_ids);
    TEST_RUN(test_pubkey_default);
    TEST_RUN(test_pubkey_from_bytes);
    TEST_RUN(test_length_is_enforced);
    TEST_RUN(test_signature_round_trip);
    TEST_RUN(test_hash_round_trip);
    TEST_RUN(test_string_buffers_are_large_enough);
    TEST_RUN(test_rejects_null);
})
