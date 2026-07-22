/*
 * Offline tests for the HTTP JSON-RPC client.
 *
 * idx_rpc_open does not connect, so construction, validation and resource
 * discipline are all checkable here. Live behaviour is exercised by
 * tools/rpcprobe.c.
 */
#include "rpc.h"

#include <string.h>

#include "test.h"

/* A client pointed at an unroutable host, with retrying disabled so failures
 * are immediate. */
static idx_rpc *unreachable_client(void) {
    static const char *urls[] = {"https://this-host-does-not-exist.invalid/"};

    idx_rpc_options options;
    idx_rpc_options_init(&options);
    options.urls = urls;
    options.url_count = 1;
    options.max_attempts = 1;
    options.connect_timeout_ms = 2000;
    options.timeout_ms = 4000;

    idx_rpc *rpc = NULL;
    TEST_EQ_INT(idx_rpc_open(&options, &rpc, NULL), IDX_OK);
    return rpc;
}

static void test_option_defaults(void) {
    idx_rpc_options options;
    memset(&options, 0xAA, sizeof(options));
    idx_rpc_options_init(&options);

    TEST_ASSERT(options.urls == NULL);
    TEST_EQ_UINT(options.url_count, 0u);
    TEST_ASSERT(options.verify_tls);
    TEST_ASSERT(options.max_attempts >= 1);
    TEST_ASSERT(options.timeout_ms > options.connect_timeout_ms);
    TEST_ASSERT(options.backoff_max_ms >= options.backoff_initial_ms);
    TEST_ASSERT(options.blocks_range_limit > 0);

    idx_rpc_options_init(NULL);
}

static void test_block_option_defaults(void) {
    idx_rpc_block_options options;
    idx_rpc_block_options_init(&options);

    /* These defaults are what the pipeline ingests; changing one changes what
     * every fetch returns. */
    TEST_EQ_STR(options.encoding, "json");
    TEST_EQ_STR(options.transaction_details, "full");
    TEST_EQ_STR(options.commitment, "confirmed");
    TEST_ASSERT(!options.rewards);
    TEST_EQ_UINT(options.max_supported_transaction_version, 0u);

    idx_rpc_block_options_init(NULL);
}

static void test_open_requires_an_endpoint(void) {
    idx_rpc *rpc = NULL;
    idx_error err;

    idx_rpc_options options;
    idx_rpc_options_init(&options);

    idx_error_clear(&err);
    TEST_EQ_INT(idx_rpc_open(&options, &rpc, &err), IDX_ERR_INVALID_ARG);
    TEST_ASSERT(strstr(err.message, "endpoint") != NULL);

    static const char *urls[] = {"https://example.invalid/"};
    options.urls = urls;
    options.url_count = 0;
    TEST_EQ_INT(idx_rpc_open(&options, &rpc, NULL), IDX_ERR_INVALID_ARG);

    options.url_count = 1;
    TEST_EQ_INT(idx_rpc_open(&options, NULL, NULL), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_rpc_open(NULL, &rpc, NULL), IDX_ERR_INVALID_ARG);
    TEST_ASSERT(rpc == NULL);
}

/* The client copies the URLs, so the caller's strings need not outlive it. */
static void test_urls_are_copied(void) {
    char first[64];
    char second[64];
    strcpy(first, "https://one.invalid/");
    strcpy(second, "https://two.invalid/");
    const char *urls[] = {first, second};

    idx_rpc_options options;
    idx_rpc_options_init(&options);
    options.urls = urls;
    options.url_count = 2;

    idx_rpc *rpc = NULL;
    TEST_EQ_INT(idx_rpc_open(&options, &rpc, NULL), IDX_OK);

    memset(first, 'x', sizeof(first));
    memset(second, 'x', sizeof(second));
    TEST_EQ_STR(idx_rpc_current_url(rpc), "https://one.invalid/");

    idx_rpc_close(rpc);
}

static void test_call_rejects_bad_arguments(void) {
    idx_rpc *rpc = unreachable_client();
    idx_rpc_response response;

    TEST_EQ_INT(idx_rpc_call(rpc, NULL, NULL, &response, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_rpc_call(rpc, "getSlot", NULL, NULL, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_rpc_call(NULL, "getSlot", NULL, &response, NULL),
                IDX_ERR_INVALID_ARG);

    idx_rpc_close(rpc);
}

/* An unreachable endpoint must surface as a network error, not a hang. */
static void test_unreachable_endpoint(void) {
    idx_rpc *rpc = unreachable_client();

    idx_error err;
    idx_error_clear(&err);
    uint64_t slot = 0;
    idx_status status = idx_rpc_get_slot(rpc, "confirmed", &slot, &err);

    TEST_CHECK(status == IDX_ERR_NETWORK, "expected a network error, got %s",
               idx_status_str(status));
    TEST_ASSERT(err.message[0] != '\0');

    idx_rpc_stats stats;
    idx_rpc_get_stats(rpc, &stats);
    TEST_EQ_UINT(stats.requests, 0u); /* nothing succeeded */

    idx_rpc_close(rpc);
}

static void test_get_blocks_validates_range(void) {
    idx_rpc *rpc = unreachable_client();

    idx_vec slots;
    idx_vec_init(&slots, sizeof(uint64_t));

    idx_error err;
    idx_error_clear(&err);
    /* Caught before any request is attempted. */
    TEST_EQ_INT(idx_rpc_get_blocks(rpc, 100, 99, NULL, &slots, &err),
                IDX_ERR_RANGE);
    TEST_ASSERT(strstr(err.message, "before") != NULL);

    TEST_EQ_INT(idx_rpc_get_blocks(rpc, 100, 200, NULL, NULL, NULL),
                IDX_ERR_INVALID_ARG);

    idx_vec_free(&slots);
    idx_rpc_close(rpc);
}

static void test_response_free_is_safe(void) {
    idx_rpc_response response;
    memset(&response, 0, sizeof(response));

    idx_rpc_response_free(&response);
    idx_rpc_response_free(&response); /* twice must be harmless */
    idx_rpc_response_free(NULL);
    TEST_ASSERT(response.doc == NULL);

    idx_rpc_batch batch;
    memset(&batch, 0, sizeof(batch));
    idx_rpc_batch_free(&batch);
    idx_rpc_batch_free(&batch);
    idx_rpc_batch_free(NULL);
    TEST_ASSERT(batch.doc == NULL);
    TEST_EQ_UINT(batch.count, 0u);
}

static void test_batch_rejects_bad_arguments(void) {
    idx_rpc *rpc = unreachable_client();

    idx_rpc_batch_call calls[1] = {{"getSlot", "[]"}};
    idx_rpc_batch batch;

    TEST_EQ_INT(idx_rpc_call_batch(rpc, calls, 0, &batch, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_rpc_call_batch(rpc, NULL, 1, &batch, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_rpc_call_batch(rpc, calls, 1, NULL, NULL),
                IDX_ERR_INVALID_ARG);

    idx_rpc_close(rpc);
}

static void test_accessors_tolerate_null(void) {
    idx_rpc_stats stats;
    memset(&stats, 0xFF, sizeof(stats));
    idx_rpc_get_stats(NULL, &stats);
    idx_rpc_get_stats(NULL, NULL);
    idx_rpc_close(NULL);

    TEST_EQ_STR(idx_rpc_current_url(NULL), "");
    TEST_EQ_INT(idx_rpc_get_version(NULL, NULL, 0, NULL), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_rpc_get_transaction(NULL, NULL, NULL, NULL, NULL),
                IDX_ERR_INVALID_ARG);
}

TEST_MAIN({
    TEST_RUN(test_option_defaults);
    TEST_RUN(test_block_option_defaults);
    TEST_RUN(test_open_requires_an_endpoint);
    TEST_RUN(test_urls_are_copied);
    TEST_RUN(test_call_rejects_bad_arguments);
    TEST_RUN(test_unreachable_endpoint);
    TEST_RUN(test_get_blocks_validates_range);
    TEST_RUN(test_response_free_is_safe);
    TEST_RUN(test_batch_rejects_bad_arguments);
    TEST_RUN(test_accessors_tolerate_null);
})
