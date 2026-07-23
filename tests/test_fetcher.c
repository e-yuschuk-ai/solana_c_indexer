/*
 * A worker's whole job is HTTP round trips, so what a unit test can reach is
 * the configuration it refuses to start with. The behaviour that matters — the
 * claim, the narrowing, the release of an unfinished tail — is exercised
 * through idx_gaps in test_gaps.c and against a live endpoint by hand.
 */
#include "fetcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

static void test_options_init(void) {
    idx_fetcher_options options;
    memset(&options, 0xAA, sizeof(options));
    idx_fetcher_options_init(&options);

    TEST_ASSERT(options.config == NULL);
    TEST_ASSERT(options.gaps == NULL);
    TEST_ASSERT(options.output == NULL);
    TEST_EQ_UINT(options.claim_span, IDX_FETCHER_DEFAULT_CLAIM_SPAN);
    TEST_ASSERT(options.idle_ms > 0);

    idx_fetcher_options_init(NULL); /* must not crash */
}

static void test_start_rejects_incomplete_options(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);
    snprintf(cfg.rpc_url, sizeof(cfg.rpc_url), "https://example.invalid");

    idx_gaps *gaps = NULL;
    TEST_EQ_INT(idx_gaps_new(0, &gaps, NULL), IDX_OK);

    idx_ring_options ring_options;
    idx_ring_options_init(&ring_options);
    ring_options.block_when_full = true;
    idx_ring *ring = NULL;
    TEST_EQ_INT(idx_ring_new(&ring_options, &ring, NULL), IDX_OK);

    idx_fetcher_pool *pool = NULL;
    idx_error err;
    idx_error_clear(&err);

    TEST_EQ_INT(idx_fetcher_pool_start(NULL, &pool, &err), IDX_ERR_INVALID_ARG);

    idx_fetcher_options options;
    idx_fetcher_options_init(&options);
    TEST_EQ_INT(idx_fetcher_pool_start(&options, NULL, &err),
                IDX_ERR_INVALID_ARG);

    /* Each required field in turn. */
    options.gaps = gaps;
    options.output = ring;
    TEST_EQ_INT(idx_fetcher_pool_start(&options, &pool, &err),
                IDX_ERR_INVALID_ARG);

    options.config = &cfg;
    options.gaps = NULL;
    TEST_EQ_INT(idx_fetcher_pool_start(&options, &pool, &err),
                IDX_ERR_INVALID_ARG);

    options.gaps = gaps;
    options.output = NULL;
    TEST_EQ_INT(idx_fetcher_pool_start(&options, &pool, &err),
                IDX_ERR_INVALID_ARG);

    /* Nothing to recover gaps from. */
    options.output = ring;
    cfg.rpc_url[0] = '\0';
    TEST_EQ_INT(idx_fetcher_pool_start(&options, &pool, &err),
                IDX_ERR_INVALID_ARG);
    TEST_ASSERT(err.message[0] != '\0');

    idx_ring_free(ring);
    idx_gaps_free(gaps);
}

/*
 * Starting and stopping without any work must not hang: the workers connect
 * lazily and spend their time idling on a condition variable, and the stop has
 * to reach them there.
 */
static void test_start_and_stop_idle(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);
    snprintf(cfg.rpc_url, sizeof(cfg.rpc_url), "https://example.invalid");
    cfg.concurrency = 2;

    idx_gaps *gaps = NULL;
    TEST_EQ_INT(idx_gaps_new(0, &gaps, NULL), IDX_OK);

    idx_ring_options ring_options;
    idx_ring_options_init(&ring_options);
    ring_options.block_when_full = true;
    idx_ring *ring = NULL;
    TEST_EQ_INT(idx_ring_new(&ring_options, &ring, NULL), IDX_OK);

    idx_fetcher_options options;
    idx_fetcher_options_init(&options);
    options.config = &cfg;
    options.gaps = gaps;
    options.output = ring;
    options.idle_ms = 10;

    idx_fetcher_pool *pool = NULL;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_fetcher_pool_start(&options, &pool, &err), IDX_OK);

    idx_fetcher_stats stats;
    memset(&stats, 0xAA, sizeof(stats));
    idx_fetcher_pool_get_stats(pool, &stats);
    TEST_EQ_UINT(stats.workers, 2u);
    TEST_EQ_UINT(stats.ranges_claimed, 0u);
    TEST_EQ_UINT(stats.blocks_fetched, 0u);

    idx_fetcher_pool_stop(pool);
    idx_fetcher_pool_stop(NULL);
    idx_fetcher_pool_get_stats(NULL, NULL);

    idx_ring_free(ring);
    idx_gaps_free(gaps);
}

TEST_MAIN({
    TEST_RUN(test_options_init);
    TEST_RUN(test_start_rejects_incomplete_options);
    TEST_RUN(test_start_and_stop_idle);
})
