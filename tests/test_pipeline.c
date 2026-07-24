/*
 * The pipeline's run loop needs a live endpoint, so what is covered here is
 * everything around it: the shape of the notifications it reads, and the
 * configuration it refuses to start with. idx_pipeline_open deliberately
 * connects to nothing, which is what makes that half testable at all.
 */
#include "pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ring.h"
#include "test.h"

static idx_json_doc *parse(const char *text) {
    idx_json_doc *doc = NULL;
    idx_error err;
    idx_error_clear(&err);
    idx_status status = idx_json_parse(idx_slice_from_str(text), &doc, &err);
    TEST_CHECK(status == IDX_OK, "parse failed: %s", err.message);
    return doc;
}

/* A notification result, trimmed to the fields the pipeline reads. */
static const char *const BLOCK_RESULT =
    "{\"context\":{\"slot\":434543707},"
    " \"value\":{\"slot\":434543707,\"err\":null,"
    "  \"block\":{\"blockhash\":\"abc\",\"parentSlot\":434543706,"
    "   \"transactions\":[{},{},{}]}}}";

static idx_status read_block(const char *text, idx_slot *slot,
                             idx_json_val *block, idx_json_doc **doc) {
    *doc = parse(text);
    idx_error err;
    idx_error_clear(&err);
    return idx_pipeline_read_block_notification(idx_json_root(*doc), slot,
                                                block, &err);
}

static void test_read_block_notification(void) {
    idx_slot slot = IDX_SLOT_NONE;
    idx_json_val block = {NULL};
    idx_json_doc *doc = NULL;

    TEST_EQ_INT(read_block(BLOCK_RESULT, &slot, &block, &doc), IDX_OK);
    TEST_EQ_UINT(slot, 434543707u);
    TEST_ASSERT(idx_json_is_object(block));
    TEST_EQ_UINT(idx_json_array_size(idx_json_get(block, "transactions")), 3u);

    idx_json_free(doc);
}

/* A provider that omits the context still carries the slot in the value. */
static void test_read_block_notification_without_context(void) {
    idx_slot slot = IDX_SLOT_NONE;
    idx_json_val block = {NULL};
    idx_json_doc *doc = NULL;

    TEST_EQ_INT(read_block("{\"value\":{\"slot\":42,\"block\":{}}}", &slot,
                           &block, &doc),
                IDX_OK);
    TEST_EQ_UINT(slot, 42u);

    idx_json_free(doc);
}

/* Slot 0 is genesis, a real slot, and must not read as "no slot". */
static void test_read_block_notification_slot_zero(void) {
    idx_slot slot = IDX_SLOT_NONE;
    idx_json_val block = {NULL};
    idx_json_doc *doc = NULL;

    TEST_EQ_INT(read_block("{\"context\":{\"slot\":0},"
                           " \"value\":{\"slot\":0,\"block\":{}}}",
                           &slot, &block, &doc),
                IDX_OK);
    TEST_EQ_UINT(slot, 0u);

    idx_json_free(doc);
}

/*
 * A skipped slot is not-found rather than an error, and the slot is still
 * reported: the pipeline has to observe it to keep the cursor honest.
 */
static void test_read_block_notification_skipped(void) {
    idx_slot slot = IDX_SLOT_NONE;
    idx_json_val block = {NULL};
    idx_json_doc *doc = NULL;

    TEST_EQ_INT(read_block("{\"context\":{\"slot\":99},\"value\":{\"slot\":99}}",
                           &slot, &block, &doc),
                IDX_ERR_NOT_FOUND);
    TEST_EQ_UINT(slot, 99u);
    idx_json_free(doc);

    /* An explicit null block reads the same way. */
    slot = IDX_SLOT_NONE;
    TEST_EQ_INT(read_block("{\"context\":{\"slot\":100},"
                           " \"value\":{\"slot\":100,\"block\":null}}",
                           &slot, &block, &doc),
                IDX_ERR_NOT_FOUND);
    TEST_EQ_UINT(slot, 100u);
    idx_json_free(doc);
}

static void test_read_block_notification_with_error(void) {
    idx_slot slot = IDX_SLOT_NONE;
    idx_json_val block = {NULL};
    idx_json_doc *doc = NULL;

    TEST_EQ_INT(read_block("{\"context\":{\"slot\":7},"
                           " \"value\":{\"slot\":7,\"err\":\"boom\","
                           "  \"block\":{}}}",
                           &slot, &block, &doc),
                IDX_ERR_NOT_FOUND);
    TEST_EQ_UINT(slot, 7u);

    idx_json_free(doc);
}

static void test_read_block_notification_malformed(void) {
    idx_slot slot = IDX_SLOT_NONE;
    idx_json_val block = {NULL};
    idx_json_doc *doc = NULL;

    /* Not an object at all. */
    TEST_EQ_INT(read_block("[1,2,3]", &slot, &block, &doc), IDX_ERR_PARSE);
    idx_json_free(doc);

    /* No slot anywhere. */
    TEST_EQ_INT(read_block("{\"value\":{\"block\":{}}}", &slot, &block, &doc),
                IDX_ERR_PARSE);
    idx_json_free(doc);

    /* A slot, but no value object to find a block in. */
    TEST_EQ_INT(read_block("{\"context\":{\"slot\":5}}", &slot, &block, &doc),
                IDX_ERR_PARSE);
    TEST_EQ_UINT(slot, 5u);
    idx_json_free(doc);
}

static void test_read_block_notification_null_output(void) {
    idx_json_doc *doc = parse(BLOCK_RESULT);
    idx_json_val block = {NULL};
    idx_slot slot = 0;

    TEST_EQ_INT(idx_pipeline_read_block_notification(idx_json_root(doc), NULL,
                                                     &block, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_pipeline_read_block_notification(idx_json_root(doc), &slot,
                                                     NULL, NULL),
                IDX_ERR_INVALID_ARG);

    idx_json_free(doc);
}

static void test_read_slot_notification(void) {
    idx_json_doc *doc = parse("{\"parent\":74,\"root\":42,\"slot\":75}");
    idx_slot slot = IDX_SLOT_NONE;
    idx_error err;
    idx_error_clear(&err);

    TEST_EQ_INT(
        idx_pipeline_read_slot_notification(idx_json_root(doc), &slot, &err),
        IDX_OK);
    TEST_EQ_UINT(slot, 75u);
    idx_json_free(doc);

    doc = parse("{\"parent\":74,\"root\":42}");
    TEST_EQ_INT(
        idx_pipeline_read_slot_notification(idx_json_root(doc), &slot, NULL),
        IDX_ERR_PARSE);
    idx_json_free(doc);

    doc = parse("\"not an object\"");
    TEST_EQ_INT(
        idx_pipeline_read_slot_notification(idx_json_root(doc), &slot, NULL),
        IDX_ERR_PARSE);
    idx_json_free(doc);

    doc = parse("{\"slot\":75}");
    TEST_EQ_INT(
        idx_pipeline_read_slot_notification(idx_json_root(doc), NULL, NULL),
        IDX_ERR_INVALID_ARG);
    idx_json_free(doc);
}

static void test_block_origin_name(void) {
    TEST_EQ_STR(idx_block_origin_name(IDX_BLOCK_FROM_SUBSCRIPTION),
                "blockSubscribe");
    TEST_EQ_STR(idx_block_origin_name(IDX_BLOCK_FROM_RPC), "getBlock");
}

static void test_options_init(void) {
    idx_pipeline_options options;
    memset(&options, 0xAA, sizeof(options));
    idx_pipeline_options_init(&options);

    TEST_ASSERT(options.config == NULL);
    TEST_ASSERT(options.cursor == NULL);
    TEST_ASSERT(options.handler == NULL);
    TEST_ASSERT(options.user == NULL);
    TEST_EQ_INT(options.poll_timeout_ms, 1000);
    TEST_EQ_INT(options.save_interval_ms, 1000);
    TEST_ASSERT(options.allow_fallback);
    TEST_ASSERT(options.recover_gaps);

    idx_pipeline_options_init(NULL); /* must not crash */
}

static idx_status accept_block(const idx_raw_block *block, void *user,
                               idx_error *err) {
    (void)err;
    *(uint64_t *)user += block->slot;
    return IDX_OK;
}

/* A config the pipeline is willing to start with. */
static void ready_config(idx_config *cfg) {
    idx_config_defaults(cfg);
    snprintf(cfg->wss_url, sizeof(cfg->wss_url), "wss://example.invalid");
    snprintf(cfg->rpc_url, sizeof(cfg->rpc_url), "https://example.invalid");
}

static void test_open_rejects_incomplete_options(void) {
    idx_config cfg;
    ready_config(&cfg);

    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 0);

    idx_pipeline *pipeline = NULL;
    idx_error err;
    idx_error_clear(&err);

    TEST_EQ_INT(idx_pipeline_open(NULL, &pipeline, &err), IDX_ERR_INVALID_ARG);

    idx_pipeline_options options;
    idx_pipeline_options_init(&options);
    TEST_EQ_INT(idx_pipeline_open(&options, NULL, &err), IDX_ERR_INVALID_ARG);

    /* Every required field, one at a time. */
    options.cursor = &cursor;
    options.handler = accept_block;
    TEST_EQ_INT(idx_pipeline_open(&options, &pipeline, &err),
                IDX_ERR_INVALID_ARG);

    options.config = &cfg;
    options.cursor = NULL;
    TEST_EQ_INT(idx_pipeline_open(&options, &pipeline, &err),
                IDX_ERR_INVALID_ARG);

    options.cursor = &cursor;
    options.handler = NULL;
    TEST_EQ_INT(idx_pipeline_open(&options, &pipeline, &err),
                IDX_ERR_INVALID_ARG);

    /* No endpoint to subscribe to. */
    options.handler = accept_block;
    cfg.wss_url[0] = '\0';
    TEST_EQ_INT(idx_pipeline_open(&options, &pipeline, &err),
                IDX_ERR_INVALID_ARG);
    TEST_ASSERT(err.message[0] != '\0');
}

static void test_open_and_close(void) {
    idx_config cfg;
    ready_config(&cfg);

    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 0);
    idx_slot_cursor_record_indexed(&cursor, 250000000u);

    uint64_t committed = 0;
    idx_pipeline_options options;
    idx_pipeline_options_init(&options);
    options.config = &cfg;
    options.cursor = &cursor;
    options.handler = accept_block;
    options.user = &committed;

    idx_pipeline *pipeline = NULL;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_pipeline_open(&options, &pipeline, &err), IDX_OK);
    TEST_ASSERT(pipeline != NULL);

    idx_pipeline_stats stats;
    memset(&stats, 0xAA, sizeof(stats));
    idx_pipeline_get_stats(pipeline, &stats);
    TEST_EQ_UINT(stats.blocks, 0u);
    TEST_EQ_UINT(stats.slots_skipped, 0u);
    TEST_EQ_UINT(stats.slots_missed, 0u);
    TEST_EQ_UINT(stats.bytes, 0u);
    TEST_ASSERT(!stats.used_fallback);
    /* The ring exists from open, sized by the config. */
    TEST_EQ_UINT(stats.queue_dropped, 0u);
    TEST_EQ_UINT(stats.queue_high_water, 0u);
    TEST_EQ_UINT(stats.queue_depth, IDX_RING_DEFAULT_DEPTH);
    /* The gap set exists from open, and starts with nothing outstanding. */
    TEST_EQ_UINT(stats.gap_slots_outstanding, 0u);
    TEST_EQ_UINT(stats.gap_ranges, 0u);
    TEST_EQ_UINT(stats.blocks_recovered, 0u);
    /* Where the cursor already was, not a fresh zero. */
    TEST_EQ_UINT(stats.last_indexed, 250000000u);
    /* Nothing has been committed, so there is no tip to measure lag against —
     * and slot 0 would read as one. */
    TEST_EQ_UINT(stats.tip_slot, IDX_SLOT_NONE);
    TEST_ASSERT(!stats.has_tip_block_time);

    /* Requesting a stop before the run starts is legal and is what a signal
     * arriving early does. */
    idx_pipeline_request_stop(pipeline);
    idx_pipeline_request_stop(NULL);

    idx_pipeline_close(pipeline);
    idx_pipeline_close(NULL);
}

/* The configured depth reaches the ring. */
static void test_queue_depth_is_configured(void) {
    idx_config cfg;
    ready_config(&cfg);
    cfg.queue_depth = 3;

    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 0);

    idx_pipeline_options options;
    idx_pipeline_options_init(&options);
    options.config = &cfg;
    options.cursor = &cursor;
    options.handler = accept_block;

    idx_pipeline *pipeline = NULL;
    TEST_EQ_INT(idx_pipeline_open(&options, &pipeline, NULL), IDX_OK);

    idx_pipeline_stats stats;
    idx_pipeline_get_stats(pipeline, &stats);
    TEST_EQ_UINT(stats.queue_depth, 3u);

    idx_pipeline_close(pipeline);

    /* And a depth the ring refuses is refused here rather than at run time. */
    cfg.queue_depth = 1;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_pipeline_open(&options, &pipeline, &err), IDX_ERR_RANGE);
}

static void test_open_rejects_unusable_subscription(void) {
    idx_config cfg;
    ready_config(&cfg);
    /* Longer than the params buffer can hold, so building them fails before
     * anything is connected. */
    memset(cfg.block_filter, 'k', sizeof(cfg.block_filter) - 1);
    cfg.block_filter[sizeof(cfg.block_filter) - 1] = '\0';

    idx_slot_cursor cursor;
    idx_slot_cursor_init(&cursor, 0);

    idx_pipeline_options options;
    idx_pipeline_options_init(&options);
    options.config = &cfg;
    options.cursor = &cursor;
    options.handler = accept_block;

    idx_pipeline *pipeline = NULL;
    idx_error err;
    idx_error_clear(&err);
    idx_status status = idx_pipeline_open(&options, &pipeline, &err);
    TEST_CHECK(status != IDX_OK, "expected a rejected subscription shape");
    TEST_ASSERT(err.message[0] != '\0');
}

static void test_run_rejects_null(void) {
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_pipeline_run(NULL, &err), IDX_ERR_INVALID_ARG);

    idx_pipeline_stats stats;
    memset(&stats, 0, sizeof(stats));
    idx_pipeline_get_stats(NULL, &stats); /* must not crash */
}

TEST_MAIN({
    TEST_RUN(test_read_block_notification);
    TEST_RUN(test_read_block_notification_without_context);
    TEST_RUN(test_read_block_notification_slot_zero);
    TEST_RUN(test_read_block_notification_skipped);
    TEST_RUN(test_read_block_notification_with_error);
    TEST_RUN(test_read_block_notification_malformed);
    TEST_RUN(test_read_block_notification_null_output);
    TEST_RUN(test_read_slot_notification);
    TEST_RUN(test_block_origin_name);
    TEST_RUN(test_options_init);
    TEST_RUN(test_open_rejects_incomplete_options);
    TEST_RUN(test_open_and_close);
    TEST_RUN(test_queue_depth_is_configured);
    TEST_RUN(test_open_rejects_unusable_subscription);
    TEST_RUN(test_run_rejects_null);
})
