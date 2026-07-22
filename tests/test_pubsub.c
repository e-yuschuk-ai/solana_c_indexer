/*
 * Offline tests for the PubSub layer.
 *
 * idx_pubsub_open connects eagerly, so what can be checked without a network
 * is the argument handling, the failure paths and the resource discipline.
 * The live behaviour — confirmation matching, demultiplexing, reconnection —
 * is exercised by tools/subscribe.c.
 */
#include "pubsub.h"

#include <string.h>

#include "test.h"

static void test_option_defaults(void) {
    idx_pubsub_options options;
    memset(&options, 0xAA, sizeof(options));
    idx_pubsub_options_init(&options);

    TEST_ASSERT(options.url == NULL);
    TEST_ASSERT(options.verify_tls);
    TEST_ASSERT(options.reconnect_initial_ms > 0);
    TEST_ASSERT(options.reconnect_max_ms >= options.reconnect_initial_ms);

    /* Solana produces a block every ~400 ms, so the idle timeout has to be
     * far longer than that to avoid reconnecting during a normal lull. */
    TEST_ASSERT(options.idle_timeout_ms > 5000);

    idx_pubsub_options_init(NULL); /* must not crash */
}

static void test_open_rejects_bad_arguments(void) {
    idx_pubsub *pubsub = NULL;
    idx_error err;

    idx_pubsub_options options;
    idx_pubsub_options_init(&options);

    idx_error_clear(&err);
    TEST_EQ_INT(idx_pubsub_open(&options, &pubsub, &err), IDX_ERR_INVALID_ARG);
    TEST_ASSERT(strstr(err.message, "url") != NULL);

    options.url = "wss://example.invalid/";
    TEST_EQ_INT(idx_pubsub_open(&options, NULL, NULL), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_pubsub_open(NULL, &pubsub, NULL), IDX_ERR_INVALID_ARG);
    TEST_ASSERT(pubsub == NULL);
}

/*
 * A failed connect must free everything it allocated, which is what the
 * sanitizers verify by running this at all.
 */
static void test_open_failure_releases_everything(void) {
    idx_pubsub_options options;
    idx_pubsub_options_init(&options);
    options.url = "wss://this-host-does-not-exist.invalid/";
    options.max_message_bytes = 65536;

    for (int attempt = 0; attempt < 3; attempt++) {
        idx_pubsub *pubsub = NULL;
        idx_error err;
        idx_error_clear(&err);

        idx_status status = idx_pubsub_open(&options, &pubsub, &err);
        TEST_CHECK(status != IDX_OK, "connecting to an invalid host succeeded");
        TEST_ASSERT(pubsub == NULL);
        TEST_ASSERT(err.message[0] != '\0');
    }
}

static void test_message_free_is_safe(void) {
    idx_pubsub_message message;
    memset(&message, 0, sizeof(message));

    /* Freeing an empty message, and freeing twice, must both be harmless. */
    idx_pubsub_message_free(&message);
    idx_pubsub_message_free(&message);
    idx_pubsub_message_free(NULL);

    TEST_ASSERT(message.doc == NULL);
    TEST_EQ_UINT(message.subscription, 0u);
    TEST_EQ_UINT(message.raw.len, 0u);
}

static void test_accessors_tolerate_null(void) {
    idx_pubsub_stats stats;
    memset(&stats, 0xFF, sizeof(stats));

    idx_pubsub_get_stats(NULL, &stats);
    idx_pubsub_get_stats(NULL, NULL);
    idx_pubsub_close(NULL);

    idx_pubsub_message message;
    TEST_EQ_INT(idx_pubsub_poll(NULL, 0, &message, NULL), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_pubsub_subscribe(NULL, "m", NULL, NULL, NULL),
                IDX_ERR_INVALID_ARG);
}

TEST_MAIN({
    TEST_RUN(test_option_defaults);
    TEST_RUN(test_open_rejects_bad_arguments);
    TEST_RUN(test_open_failure_releases_everything);
    TEST_RUN(test_message_free_is_safe);
    TEST_RUN(test_accessors_tolerate_null);
})
