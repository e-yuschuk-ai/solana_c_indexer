/*
 * Offline tests for the WebSocket client.
 *
 * Anything requiring a live endpoint lives in tools/wsdump.c instead, so
 * `make test` stays runnable without network access or credentials.
 */
#include "ws.h"

#include <string.h>

#include "test.h"

static void test_option_defaults(void) {
    idx_ws_options options;
    memset(&options, 0xAA, sizeof(options));
    idx_ws_options_init(&options);

    TEST_ASSERT(options.url == NULL);
    TEST_ASSERT(options.verify_tls); /* must default to on */
    TEST_ASSERT(options.connect_timeout_ms > 0);

    /* The cap must clear the largest blocks seen on mainnet with room to
     * spare; 11 MiB has been observed. */
    TEST_ASSERT(options.max_message_bytes >= 32u * 1024u * 1024u);
    TEST_ASSERT(options.initial_buffer_bytes > 0);
    TEST_ASSERT(options.initial_buffer_bytes <= options.max_message_bytes);

    idx_ws_options_init(NULL); /* must not crash */
}

static void test_connect_rejects_bad_arguments(void) {
    idx_ws *ws = NULL;
    idx_error err;

    idx_ws_options options;
    idx_ws_options_init(&options);

    idx_error_clear(&err);
    TEST_EQ_INT(idx_ws_connect(&options, &ws, &err), IDX_ERR_INVALID_ARG);
    TEST_ASSERT(strstr(err.message, "url") != NULL);

    options.url = "wss://example.invalid/";
    TEST_EQ_INT(idx_ws_connect(&options, NULL, NULL), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_ws_connect(NULL, &ws, NULL), IDX_ERR_INVALID_ARG);
    TEST_ASSERT(ws == NULL);
}

/* An unresolvable host must fail cleanly rather than hang or leak. */
static void test_connect_failure_is_reported(void) {
    idx_ws_options options;
    idx_ws_options_init(&options);
    options.url = "wss://this-host-does-not-exist.invalid/";
    options.connect_timeout_ms = 3000;
    /* Keep the failed attempt cheap: no large buffer should be retained. */
    options.initial_buffer_bytes = 4096;

    idx_ws *ws = NULL;
    idx_error err;
    idx_error_clear(&err);

    idx_status status = idx_ws_connect(&options, &ws, &err);
    TEST_CHECK(status == IDX_ERR_NETWORK, "expected a network error, got %s",
               idx_status_str(status));
    TEST_ASSERT(ws == NULL);
    TEST_ASSERT(err.message[0] != '\0');
}

/* A scheme libcurl does not treat as WebSocket must be refused. */
static void test_rejects_non_websocket_scheme(void) {
    idx_ws_options options;
    idx_ws_options_init(&options);
    options.url = "gopher://example.invalid/";
    options.connect_timeout_ms = 3000;
    options.initial_buffer_bytes = 4096;

    idx_ws *ws = NULL;
    TEST_ASSERT(idx_ws_connect(&options, &ws, NULL) != IDX_OK);
    TEST_ASSERT(ws == NULL);
}

static void test_accessors_tolerate_null(void) {
    idx_ws_stats stats;
    memset(&stats, 0xFF, sizeof(stats));

    idx_ws_get_stats(NULL, &stats);
    idx_ws_get_stats(NULL, NULL);
    TEST_EQ_UINT(idx_ws_buffer_capacity(NULL), 0u);

    idx_ws_close(NULL); /* must not crash */

    idx_slice message;
    TEST_EQ_INT(idx_ws_recv(NULL, 0, &message, NULL), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_ws_send_text(NULL, idx_slice_from_str("x"), NULL),
                IDX_ERR_INVALID_ARG);
}

/* The transport statuses must be distinguishable and named. */
static void test_transport_statuses(void) {
    TEST_EQ_STR(idx_status_str(IDX_ERR_NETWORK), "network error");
    TEST_EQ_STR(idx_status_str(IDX_ERR_TIMEOUT), "timed out");
    TEST_EQ_STR(idx_status_str(IDX_ERR_CLOSED), "connection closed");
    TEST_EQ_STR(idx_status_str(IDX_ERR_REMOTE), "remote error");

    TEST_ASSERT(IDX_ERR_TIMEOUT != IDX_ERR_CLOSED);
    TEST_ASSERT(IDX_ERR_NETWORK != IDX_ERR_REMOTE);
}

TEST_MAIN({
    TEST_RUN(test_option_defaults);
    TEST_RUN(test_connect_rejects_bad_arguments);
    TEST_RUN(test_connect_failure_is_reported);
    TEST_RUN(test_rejects_non_websocket_scheme);
    TEST_RUN(test_accessors_tolerate_null);
    TEST_RUN(test_transport_statuses);
})
