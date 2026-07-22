#include "json.h"

#include <stdlib.h>
#include <string.h>

#include "test.h"

/* Shaped like a real getBlock response, trimmed to what the tests need. */
static const char k_response[] =
    "{"
    "  \"jsonrpc\": \"2.0\","
    "  \"result\": {"
    "    \"blockHeight\": 412345678,"
    "    \"blockTime\": 1721650000,"
    "    \"blockhash\": \"BLOCKHASHvalue\","
    "    \"parentSlot\": 434543706,"
    "    \"rewards\": [],"
    "    \"transactions\": ["
    "      {"
    "        \"meta\": {"
    "          \"err\": null,"
    "          \"fee\": 5000,"
    "          \"preBalances\": [1000000, 2000000],"
    "          \"postBalances\": [995000, 2000000]"
    "        },"
    "        \"transaction\": {"
    "          \"signatures\": [\"SIGNATUREone\"]"
    "        }"
    "      },"
    "      {"
    "        \"meta\": { \"err\": {\"InstructionError\": [0, \"Custom\"]},"
    "                    \"fee\": 5001 },"
    "        \"transaction\": { \"signatures\": [\"SIGNATUREtwo\"] }"
    "      }"
    "    ]"
    "  },"
    "  \"id\": 1"
    "}";

static idx_json_doc *parse_response(void) {
    idx_json_doc *doc = NULL;
    idx_status status = idx_json_parse(
        idx_slice_make(k_response, strlen(k_response)), &doc, NULL);
    TEST_EQ_INT(status, IDX_OK);
    return doc;
}

static void test_parse_and_navigate(void) {
    idx_json_doc *doc = parse_response();
    if (doc == NULL) {
        return;
    }

    idx_json_val root = idx_json_root(doc);
    TEST_ASSERT(idx_json_is_object(root));

    idx_json_val result;
    TEST_EQ_INT(idx_json_read_object(root, "result", &result, NULL), IDX_OK);

    uint64_t height = 0;
    TEST_EQ_INT(idx_json_read_u64(result, "blockHeight", &height, NULL), IDX_OK);
    TEST_EQ_UINT(height, 412345678u);

    uint64_t parent = 0;
    TEST_EQ_INT(idx_json_read_u64(result, "parentSlot", &parent, NULL), IDX_OK);
    TEST_EQ_UINT(parent, 434543706u);

    idx_slice blockhash;
    TEST_EQ_INT(idx_json_read_string(result, "blockhash", &blockhash, NULL),
                IDX_OK);
    TEST_ASSERT(idx_slice_equal(blockhash, idx_slice_from_str("BLOCKHASHvalue")));

    idx_json_free(doc);
}

static void test_arrays(void) {
    idx_json_doc *doc = parse_response();
    if (doc == NULL) {
        return;
    }

    idx_json_val result = idx_json_get(idx_json_root(doc), "result");
    idx_json_val transactions;
    TEST_EQ_INT(idx_json_read_array(result, "transactions", &transactions, NULL),
                IDX_OK);
    TEST_EQ_UINT(idx_json_array_size(transactions), 2u);

    /* Empty arrays are arrays, not absent values. */
    idx_json_val rewards = idx_json_get(result, "rewards");
    TEST_ASSERT(idx_json_is_array(rewards));
    TEST_EQ_UINT(idx_json_array_size(rewards), 0u);

    idx_json_val first = idx_json_array_get(transactions, 0);
    TEST_ASSERT(idx_json_is_object(first));
    TEST_ASSERT(!idx_json_is_present(idx_json_array_get(transactions, 2)));

    uint64_t fee = 0;
    TEST_EQ_INT(idx_json_read_u64(idx_json_get(first, "meta"), "fee", &fee,
                                  NULL),
                IDX_OK);
    TEST_EQ_UINT(fee, 5000u);

    /* Iteration must visit every element exactly once. */
    size_t cursor = 0;
    idx_json_val item;
    size_t visited = 0;
    uint64_t fees[2] = {0};
    while (idx_json_array_next(transactions, &cursor, &item)) {
        TEST_EQ_INT(idx_json_read_u64(idx_json_get(item, "meta"), "fee",
                                      &fees[visited], NULL),
                    IDX_OK);
        visited++;
    }
    TEST_EQ_UINT(visited, 2u);
    TEST_EQ_UINT(fees[0], 5000u);
    TEST_EQ_UINT(fees[1], 5001u);

    idx_json_free(doc);
}

static void test_object_iteration(void) {
    idx_json_doc *doc = NULL;
    const char *json = "{\"a\":1,\"b\":2,\"c\":3}";
    TEST_EQ_INT(idx_json_parse(idx_slice_from_str(json), &doc, NULL), IDX_OK);
    if (doc == NULL) {
        return;
    }

    size_t cursor = 0;
    idx_slice key;
    idx_json_val value;
    size_t visited = 0;
    uint64_t total = 0;

    while (idx_json_object_next(idx_json_root(doc), &cursor, &key, &value)) {
        TEST_EQ_UINT(key.len, 1u);
        uint64_t number = 0;
        TEST_EQ_INT(idx_json_as_u64(value, "member", &number, NULL), IDX_OK);
        total += number;
        visited++;
    }

    TEST_EQ_UINT(visited, 3u);
    TEST_EQ_UINT(total, 6u);

    idx_json_free(doc);
}

/* Missing and wrong-typed fields must be distinguishable and well described. */
static void test_read_errors(void) {
    idx_json_doc *doc = parse_response();
    if (doc == NULL) {
        return;
    }

    idx_json_val result = idx_json_get(idx_json_root(doc), "result");
    idx_error err;
    uint64_t number = 0;

    idx_error_clear(&err);
    TEST_EQ_INT(idx_json_read_u64(result, "nonexistent", &number, &err),
                IDX_ERR_NOT_FOUND);
    TEST_ASSERT(strstr(err.message, "nonexistent") != NULL);

    idx_error_clear(&err);
    TEST_EQ_INT(idx_json_read_u64(result, "blockhash", &number, &err),
                IDX_ERR_PARSE);
    TEST_ASSERT(strstr(err.message, "is string") != NULL);
    TEST_ASSERT(strstr(err.message, "unsigned integer") != NULL);

    idx_slice text;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_json_read_string(result, "blockHeight", &text, &err),
                IDX_ERR_PARSE);
    TEST_ASSERT(strstr(err.message, "is number") != NULL);

    idx_json_val nested;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_json_read_object(result, "transactions", &nested, &err),
                IDX_ERR_PARSE);
    TEST_ASSERT(strstr(err.message, "is array") != NULL);

    idx_json_free(doc);
}

/* JSON null is present but distinct from a missing field. */
static void test_null_handling(void) {
    idx_json_doc *doc = parse_response();
    if (doc == NULL) {
        return;
    }

    idx_json_val first = idx_json_array_get(
        idx_json_get(idx_json_get(idx_json_root(doc), "result"), "transactions"),
        0);
    idx_json_val meta = idx_json_get(first, "meta");

    idx_json_val error_field = idx_json_get(meta, "err");
    TEST_ASSERT(idx_json_is_present(error_field));
    TEST_ASSERT(idx_json_is_null(error_field));
    TEST_EQ_STR(idx_json_type_name(error_field), "null");

    idx_json_val absent = idx_json_get(meta, "notThere");
    TEST_ASSERT(!idx_json_is_present(absent));
    TEST_ASSERT(!idx_json_is_null(absent));
    TEST_EQ_STR(idx_json_type_name(absent), "absent");

    /* A successful transaction has err == null; a failed one has an object. */
    idx_json_val second = idx_json_array_get(
        idx_json_get(idx_json_get(idx_json_root(doc), "result"), "transactions"),
        1);
    TEST_ASSERT(idx_json_is_object(idx_json_get(idx_json_get(second, "meta"),
                                                "err")));

    idx_json_free(doc);
}

static void test_optional_readers(void) {
    idx_json_doc *doc = NULL;
    const char *json =
        "{\"present\":42,\"nulled\":null,\"text\":\"hi\",\"flag\":true}";
    TEST_EQ_INT(idx_json_parse(idx_slice_from_str(json), &doc, NULL), IDX_OK);
    if (doc == NULL) {
        return;
    }
    idx_json_val root = idx_json_root(doc);

    uint64_t number = 7;
    TEST_ASSERT(idx_json_opt_u64(root, "present", &number));
    TEST_EQ_UINT(number, 42u);

    /* Absent and null both report false and leave the output alone. */
    number = 7;
    TEST_ASSERT(!idx_json_opt_u64(root, "missing", &number));
    TEST_EQ_UINT(number, 7u);
    TEST_ASSERT(!idx_json_opt_u64(root, "nulled", &number));
    TEST_EQ_UINT(number, 7u);

    idx_slice text;
    TEST_ASSERT(idx_json_opt_string(root, "text", &text));
    TEST_ASSERT(idx_slice_equal(text, idx_slice_from_str("hi")));
    TEST_ASSERT(!idx_json_opt_string(root, "present", &text)); /* wrong type */

    bool flag = false;
    TEST_ASSERT(idx_json_opt_bool(root, "flag", &flag));
    TEST_ASSERT(flag);

    idx_json_free(doc);
}

static void test_numbers(void) {
    idx_json_doc *doc = NULL;
    const char *json =
        "{\"big\":18446744073709551615,\"neg\":-5,\"real\":1.5,\"zero\":0}";
    TEST_EQ_INT(idx_json_parse(idx_slice_from_str(json), &doc, NULL), IDX_OK);
    if (doc == NULL) {
        return;
    }
    idx_json_val root = idx_json_root(doc);

    uint64_t big = 0;
    TEST_EQ_INT(idx_json_read_u64(root, "big", &big, NULL), IDX_OK);
    TEST_EQ_UINT(big, 18446744073709551615ull);

    /* A negative number is not an unsigned one, and says so. */
    idx_error err;
    idx_error_clear(&err);
    uint64_t number = 0;
    TEST_EQ_INT(idx_json_read_u64(root, "neg", &number, &err), IDX_ERR_RANGE);
    TEST_ASSERT(strstr(err.message, "negative") != NULL);

    int64_t signed_value = 0;
    TEST_EQ_INT(idx_json_read_i64(root, "neg", &signed_value, NULL), IDX_OK);
    TEST_EQ_INT(signed_value, -5);

    double real = 0;
    TEST_EQ_INT(idx_json_read_double(root, "real", &real, NULL), IDX_OK);
    TEST_ASSERT(real > 1.4 && real < 1.6);

    uint64_t zero = 1;
    TEST_EQ_INT(idx_json_read_u64(root, "zero", &zero, NULL), IDX_OK);
    TEST_EQ_UINT(zero, 0u);

    idx_json_free(doc);
}

static void test_invalid_json(void) {
    const char *bad[] = {
        "",  "{", "{\"a\":}", "[1,2", "nope", "{\"a\":1,}", "{'a':1}",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        idx_json_doc *doc = NULL;
        idx_error err;
        idx_error_clear(&err);
        TEST_CHECK(idx_json_parse(idx_slice_from_str(bad[i]), &doc, &err) ==
                       IDX_ERR_PARSE,
                   "\"%s\" should have failed to parse", bad[i]);
        TEST_CHECK(doc == NULL, "\"%s\" left a document behind", bad[i]);
        TEST_CHECK(err.message[0] != '\0', "\"%s\" produced no message",
                   bad[i]);
    }
}

/* In-situ parsing avoids copying the multi-megabyte block payloads. */
static void test_parse_insitu(void) {
    const char *source = "{\"slot\":434543707,\"name\":\"block\"}";
    size_t len = strlen(source);
    size_t capacity = len + 4;

    char *writable = malloc(capacity);
    TEST_ASSERT(writable != NULL);
    if (writable == NULL) {
        return;
    }
    memcpy(writable, source, len);

    idx_json_doc *doc = NULL;
    TEST_EQ_INT(idx_json_parse_insitu(writable, len, capacity, &doc, NULL),
                IDX_OK);

    if (doc != NULL) {
        uint64_t slot = 0;
        TEST_EQ_INT(idx_json_read_u64(idx_json_root(doc), "slot", &slot, NULL),
                    IDX_OK);
        TEST_EQ_UINT(slot, 434543707u);
        idx_json_free(doc);
    }
    free(writable);

    /* Too little padding is a caller error, caught rather than overrun. */
    char tight[8] = "{\"a\":1}";
    idx_json_doc *rejected = NULL;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_json_parse_insitu(tight, 7, 7, &rejected, &err),
                IDX_ERR_INVALID_ARG);
    TEST_ASSERT(strstr(err.message, "capacity") != NULL);
}

static void test_escaping(void) {
    const struct {
        const char *input;
        const char *expected;
    } cases[] = {
        {"plain", "\"plain\""},
        {"with \"quotes\"", "\"with \\\"quotes\\\"\""},
        {"back\\slash", "\"back\\\\slash\""},
        {"line\nbreak", "\"line\\nbreak\""},
        {"tab\there", "\"tab\\there\""},
        {"carriage\rreturn", "\"carriage\\rreturn\""},
        {"", "\"\""},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        idx_buffer buffer;
        idx_buffer_init(&buffer);
        TEST_EQ_INT(idx_json_write_escaped(
                        &buffer, idx_slice_from_str(cases[i].input), NULL),
                    IDX_OK);
        TEST_EQ_INT(idx_buffer_append_byte(&buffer, '\0', NULL), IDX_OK);
        TEST_CHECK(strcmp((const char *)buffer.data, cases[i].expected) == 0,
                   "\"%s\": expected %s, got %s", cases[i].input,
                   cases[i].expected, (const char *)buffer.data);
        idx_buffer_free(&buffer);
    }

    /* A control character becomes a \u escape. */
    idx_buffer buffer;
    idx_buffer_init(&buffer);
    const char control[] = {'a', 0x01, 'b'};
    TEST_EQ_INT(idx_json_write_escaped(&buffer, idx_slice_make(control, 3), NULL),
                IDX_OK);
    TEST_EQ_INT(idx_buffer_append_byte(&buffer, '\0', NULL), IDX_OK);
    TEST_EQ_STR((const char *)buffer.data, "\"a\\u0001b\"");
    idx_buffer_free(&buffer);
}

/* Anything written must parse back, which is what makes escaping correct. */
static void test_escaped_output_round_trips(void) {
    const char *tricky = "quotes \" backslash \\ newline \n tab \t unicode \xc3\xa9";

    idx_buffer buffer;
    idx_buffer_init(&buffer);
    TEST_EQ_INT(
        idx_json_write_escaped(&buffer, idx_slice_from_str(tricky), NULL),
        IDX_OK);

    idx_buffer wrapped;
    idx_buffer_init(&wrapped);
    TEST_EQ_INT(idx_buffer_append(&wrapped, "{\"k\":", 5, NULL), IDX_OK);
    TEST_EQ_INT(idx_buffer_append(&wrapped, buffer.data, buffer.len, NULL),
                IDX_OK);
    TEST_EQ_INT(idx_buffer_append_byte(&wrapped, '}', NULL), IDX_OK);

    idx_json_doc *doc = NULL;
    TEST_EQ_INT(idx_json_parse(idx_buffer_slice(&wrapped), &doc, NULL), IDX_OK);
    if (doc != NULL) {
        idx_slice value;
        TEST_EQ_INT(idx_json_read_string(idx_json_root(doc), "k", &value, NULL),
                    IDX_OK);
        TEST_ASSERT(idx_slice_equal(value, idx_slice_from_str(tricky)));
        idx_json_free(doc);
    }

    idx_buffer_free(&buffer);
    idx_buffer_free(&wrapped);
}

static void test_rpc_request(void) {
    idx_buffer buffer;
    idx_buffer_init(&buffer);

    TEST_EQ_INT(idx_json_write_rpc_request(&buffer, 7, "getSlot", NULL, NULL),
                IDX_OK);
    TEST_EQ_INT(idx_buffer_append_byte(&buffer, '\0', NULL), IDX_OK);
    TEST_EQ_STR((const char *)buffer.data,
                "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"getSlot\"}");

    idx_buffer_clear(&buffer);
    TEST_EQ_INT(idx_json_write_rpc_request(&buffer, 1, "blockSubscribe",
                                           "[\"all\",{\"commitment\":"
                                           "\"confirmed\"}]",
                                           NULL),
                IDX_OK);

    /* The result must be valid JSON with the fields in place. */
    idx_json_doc *doc = NULL;
    TEST_EQ_INT(idx_json_parse(idx_buffer_slice(&buffer), &doc, NULL), IDX_OK);
    if (doc != NULL) {
        idx_json_val root = idx_json_root(doc);
        idx_slice version, method;
        uint64_t id = 0;
        TEST_EQ_INT(idx_json_read_string(root, "jsonrpc", &version, NULL),
                    IDX_OK);
        TEST_ASSERT(idx_slice_equal(version, idx_slice_from_str("2.0")));
        TEST_EQ_INT(idx_json_read_u64(root, "id", &id, NULL), IDX_OK);
        TEST_EQ_UINT(id, 1u);
        TEST_EQ_INT(idx_json_read_string(root, "method", &method, NULL), IDX_OK);
        TEST_ASSERT(idx_slice_equal(method,
                                    idx_slice_from_str("blockSubscribe")));
        TEST_EQ_UINT(idx_json_array_size(idx_json_get(root, "params")), 2u);
        idx_json_free(doc);
    }

    idx_buffer_free(&buffer);
}

/* Navigating through absent or wrongly typed values must not crash. */
static void test_navigation_is_total(void) {
    idx_json_val absent = idx_json_get(idx_json_root(NULL), "anything");
    TEST_ASSERT(!idx_json_is_present(absent));
    TEST_ASSERT(!idx_json_is_present(idx_json_get(absent, "deeper")));
    TEST_EQ_UINT(idx_json_array_size(absent), 0u);
    TEST_ASSERT(!idx_json_is_present(idx_json_array_get(absent, 0)));

    size_t cursor = 0;
    TEST_ASSERT(!idx_json_array_next(absent, &cursor, NULL));
    TEST_ASSERT(!idx_json_object_next(absent, &cursor, NULL, NULL));

    idx_json_free(NULL); /* must not crash */
}

TEST_MAIN({
    TEST_RUN(test_parse_and_navigate);
    TEST_RUN(test_arrays);
    TEST_RUN(test_object_iteration);
    TEST_RUN(test_read_errors);
    TEST_RUN(test_null_handling);
    TEST_RUN(test_optional_readers);
    TEST_RUN(test_numbers);
    TEST_RUN(test_invalid_json);
    TEST_RUN(test_parse_insitu);
    TEST_RUN(test_escaping);
    TEST_RUN(test_escaped_output_round_trips);
    TEST_RUN(test_rpc_request);
    TEST_RUN(test_navigation_is_total);
})
