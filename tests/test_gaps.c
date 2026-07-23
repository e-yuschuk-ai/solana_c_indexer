/*
 * The watermark is the part worth being careful about: it decides where a
 * restart resumes, so a wrong answer here silently skips slots that were never
 * indexed. Most of what follows is about the ways a range set can quietly get
 * that wrong.
 */
#include "gaps.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

static idx_gaps *open_gaps(size_t max_ranges) {
    idx_gaps *gaps = NULL;
    idx_error err;
    idx_error_clear(&err);
    TEST_CHECK(idx_gaps_new(max_ranges, &gaps, &err) == IDX_OK, "new: %s",
               err.message);
    return gaps;
}

static size_t range_count(const idx_gaps *gaps) {
    idx_gaps_stats stats;
    memset(&stats, 0, sizeof(stats));
    idx_gaps_get_stats(gaps, &stats);
    return stats.range_count;
}

static uint64_t slot_count(const idx_gaps *gaps) {
    idx_gaps_stats stats;
    memset(&stats, 0, sizeof(stats));
    idx_gaps_get_stats(gaps, &stats);
    return stats.slot_count;
}

static void test_add_and_merge(void) {
    idx_gaps *gaps = open_gaps(0);
    TEST_ASSERT(idx_gaps_is_empty(gaps));

    TEST_EQ_INT(idx_gaps_add(gaps, 10, 20, NULL), IDX_OK);
    TEST_EQ_UINT(range_count(gaps), 1u);
    TEST_EQ_UINT(slot_count(gaps), 11u);
    TEST_ASSERT(!idx_gaps_is_empty(gaps));

    /* Touching ranges become one. */
    TEST_EQ_INT(idx_gaps_add(gaps, 21, 25, NULL), IDX_OK);
    TEST_EQ_UINT(range_count(gaps), 1u);
    TEST_EQ_UINT(slot_count(gaps), 16u);

    /* Overlapping too. */
    TEST_EQ_INT(idx_gaps_add(gaps, 5, 12, NULL), IDX_OK);
    TEST_EQ_UINT(range_count(gaps), 1u);
    TEST_EQ_UINT(slot_count(gaps), 21u); /* 5..25 */

    /* A disjoint one stays separate. */
    TEST_EQ_INT(idx_gaps_add(gaps, 100, 100, NULL), IDX_OK);
    TEST_EQ_UINT(range_count(gaps), 2u);
    TEST_EQ_UINT(slot_count(gaps), 22u);

    idx_gaps_free(gaps);
}

/* Callers derive ranges from slot arithmetic, where "nothing missing" is the
 * normal case and must not read as an error. */
static void test_empty_range_is_not_an_error(void) {
    idx_gaps *gaps = open_gaps(0);

    TEST_EQ_INT(idx_gaps_add(gaps, 20, 10, NULL), IDX_OK);
    TEST_ASSERT(idx_gaps_is_empty(gaps));
    TEST_EQ_INT(idx_gaps_add(gaps, IDX_SLOT_NONE, IDX_SLOT_NONE, NULL), IDX_OK);
    TEST_ASSERT(idx_gaps_is_empty(gaps));

    idx_gaps_free(gaps);
}

static void test_watermark(void) {
    idx_gaps *gaps = open_gaps(0);

    /* Nothing outstanding: the committed frontier is trustworthy as-is. */
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 500), 500u);
    TEST_EQ_UINT(idx_gaps_watermark(gaps, IDX_SLOT_NONE), IDX_SLOT_NONE);

    /*
     * The case this exists for: slot 100 is missing while 101..500 committed.
     * The high-water mark says 500 and would skip 100 on a restart.
     */
    idx_gaps_add(gaps, 100, 100, NULL);
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 500), 99u);

    /* A lower gap dominates. */
    idx_gaps_add(gaps, 40, 45, NULL);
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 500), 39u);

    /* Resolving the lower one lets the watermark move up to the next. */
    idx_gaps_resolve(gaps, 40, 45);
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 500), 99u);

    idx_gaps_resolve(gaps, 100, 100);
    TEST_ASSERT(idx_gaps_is_empty(gaps));
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 500), 500u);

    /* A gap above everything committed cannot push the watermark up. */
    idx_gaps_add(gaps, 900, 950, NULL);
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 500), 500u);

    idx_gaps_free(gaps);
}

/* Genesis outstanding means nothing at all can be trusted. */
static void test_watermark_at_genesis(void) {
    idx_gaps *gaps = open_gaps(0);
    idx_gaps_add(gaps, 0, 3, NULL);
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 100), IDX_SLOT_NONE);
    idx_gaps_free(gaps);
}

/* A claim is invisible to other claimants but still holds the watermark down:
 * being worked on is not the same as being recovered. */
static void test_claim_is_exclusive_but_still_outstanding(void) {
    idx_gaps *gaps = open_gaps(0);
    idx_gaps_add(gaps, 10, 12, NULL);

    idx_gap_range claim;
    TEST_ASSERT(idx_gaps_claim(gaps, 100, &claim));
    TEST_EQ_UINT(claim.from, 10u);
    TEST_EQ_UINT(claim.to, 12u);

    /* Nobody else can take it. */
    idx_gap_range second;
    TEST_ASSERT(!idx_gaps_claim(gaps, 100, &second));

    /* But it is still missing. */
    TEST_ASSERT(!idx_gaps_is_empty(gaps));
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 500), 9u);

    idx_gaps_resolve(gaps, claim.from, claim.to);
    TEST_ASSERT(idx_gaps_is_empty(gaps));
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 500), 500u);

    idx_gaps_free(gaps);
}

/* A wide hole is split so several fetchers can work it at once. */
static void test_claim_splits_wide_ranges(void) {
    idx_gaps *gaps = open_gaps(0);
    idx_gaps_add(gaps, 1000, 1999, NULL);

    idx_gap_range first;
    idx_gap_range second;
    TEST_ASSERT(idx_gaps_claim(gaps, 100, &first));
    TEST_EQ_UINT(first.from, 1000u);
    TEST_EQ_UINT(first.to, 1099u);

    TEST_ASSERT(idx_gaps_claim(gaps, 100, &second));
    TEST_EQ_UINT(second.from, 1100u);
    TEST_EQ_UINT(second.to, 1199u);

    /* Nothing was lost in the splitting. */
    TEST_EQ_UINT(slot_count(gaps), 1000u);
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 5000), 999u);

    idx_gaps_free(gaps);
}

static void test_claim_of_nothing(void) {
    idx_gaps *gaps = open_gaps(0);
    idx_gap_range claim;
    TEST_ASSERT(!idx_gaps_claim(gaps, 10, &claim));
    TEST_ASSERT(!idx_gaps_claim(NULL, 10, &claim));
    TEST_ASSERT(!idx_gaps_claim(gaps, 10, NULL));
    idx_gaps_free(gaps);
}

static void test_resolve_partial(void) {
    idx_gaps *gaps = open_gaps(0);
    idx_gaps_add(gaps, 10, 20, NULL);

    /* A prefix. */
    idx_gaps_resolve(gaps, 10, 14);
    TEST_EQ_UINT(slot_count(gaps), 6u); /* 15..20 */
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 100), 14u);

    /* A suffix. */
    idx_gaps_resolve(gaps, 18, 20);
    TEST_EQ_UINT(slot_count(gaps), 3u); /* 15..17 */

    /* The middle, which splits the range in two. */
    idx_gaps_resolve(gaps, 16, 16);
    TEST_EQ_UINT(range_count(gaps), 2u);
    TEST_EQ_UINT(slot_count(gaps), 2u); /* 15 and 17 */
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 100), 14u);

    /* Resolving beyond the ends is harmless. */
    idx_gaps_resolve(gaps, 0, 1000);
    TEST_ASSERT(idx_gaps_is_empty(gaps));

    idx_gaps_free(gaps);
}

/* A fetch that dies part way puts the tail back rather than losing it. */
static void test_release_returns_the_tail(void) {
    idx_gaps *gaps = open_gaps(0);
    idx_gaps_add(gaps, 10, 20, NULL);

    idx_gap_range claim;
    TEST_ASSERT(idx_gaps_claim(gaps, 100, &claim));

    /* Got as far as 14, then the endpoint failed. */
    idx_gaps_resolve(gaps, 10, 14);
    TEST_EQ_INT(idx_gaps_release(gaps, 15, 20, NULL), IDX_OK);

    /* The tail is claimable again, and never stopped being outstanding. */
    TEST_EQ_UINT(slot_count(gaps), 6u);
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 100), 14u);

    idx_gap_range again;
    TEST_ASSERT(idx_gaps_claim(gaps, 100, &again));
    TEST_EQ_UINT(again.from, 15u);
    TEST_EQ_UINT(again.to, 20u);

    /* Releasing does not read as recovery. */
    idx_gaps_stats stats;
    idx_gaps_get_stats(gaps, &stats);
    TEST_EQ_UINT(stats.resolved, 5u); /* 10..14 only */

    idx_gaps_free(gaps);
}

/*
 * Past the range limit the set coarsens rather than growing without bound. It
 * must stay a superset: merging swallows indexed slots, which costs a
 * refetch, where dropping a range would lose one for good.
 */
static void test_coarsening_never_forgets(void) {
    idx_gaps *gaps = open_gaps(4);

    for (idx_slot i = 0; i < 20; i++) {
        TEST_EQ_INT(idx_gaps_add(gaps, 100 + i * 10, 100 + i * 10, NULL),
                    IDX_OK);
    }

    TEST_ASSERT(range_count(gaps) <= 4u);

    idx_gaps_stats stats;
    idx_gaps_get_stats(gaps, &stats);
    TEST_ASSERT(stats.coarsened > 0);

    /* Every slot originally recorded is still covered. */
    TEST_ASSERT(stats.slot_count >= 20u);
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 1000), 99u);
    TEST_EQ_UINT(stats.lowest, 100u);

    idx_gaps_free(gaps);
}

static void test_rejects_bad_arguments(void) {
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_gaps_new(0, NULL, &err), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_gaps_add(NULL, 1, 2, &err), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_gaps_release(NULL, 1, 2, &err), IDX_ERR_INVALID_ARG);

    TEST_ASSERT(idx_gaps_is_empty(NULL));
    TEST_EQ_UINT(idx_gaps_watermark(NULL, 42), 42u);
    idx_gaps_resolve(NULL, 1, 2);
    idx_gaps_free(NULL);
    idx_gaps_get_stats(NULL, NULL);
}

/* ------------------------------------------------------------- concurrency -- */

typedef struct {
    idx_gaps *gaps;
    uint64_t claimed;
} worker_args;

/* Fetchers claim and resolve while the pipeline keeps adding. */
static void *drain(void *argument) {
    worker_args *args = (worker_args *)argument;
    for (int spins = 0; spins < 200000; spins++) {
        idx_gap_range claim;
        if (!idx_gaps_claim(args->gaps, 8, &claim)) {
            if (idx_gaps_is_empty(args->gaps)) {
                break;
            }
            continue;
        }
        idx_gaps_resolve(args->gaps, claim.from, claim.to);
        args->claimed += claim.to - claim.from + 1;
    }
    return NULL;
}

static void test_concurrent_claim_and_resolve(void) {
    idx_gaps *gaps = open_gaps(0);

    /* 400 disjoint holes, 4 slots each. */
    for (idx_slot i = 0; i < 400; i++) {
        idx_gaps_add(gaps, 1000 + i * 10, 1000 + i * 10 + 3, NULL);
    }

    worker_args workers[4];
    pthread_t threads[4];
    for (size_t i = 0; i < 4; i++) {
        workers[i].gaps = gaps;
        workers[i].claimed = 0;
        TEST_EQ_INT(pthread_create(&threads[i], NULL, drain, &workers[i]), 0);
    }

    uint64_t total = 0;
    for (size_t i = 0; i < 4; i++) {
        TEST_EQ_INT(pthread_join(threads[i], NULL), 0);
        total += workers[i].claimed;
    }

    /* Every slot was claimed exactly once, by exactly one worker. */
    TEST_EQ_UINT(total, 1600u);
    TEST_ASSERT(idx_gaps_is_empty(gaps));
    TEST_EQ_UINT(idx_gaps_watermark(gaps, 9999), 9999u);

    idx_gaps_stats stats;
    idx_gaps_get_stats(gaps, &stats);
    TEST_EQ_UINT(stats.resolved, 1600u);

    idx_gaps_free(gaps);
}

TEST_MAIN({
    TEST_RUN(test_add_and_merge);
    TEST_RUN(test_empty_range_is_not_an_error);
    TEST_RUN(test_watermark);
    TEST_RUN(test_watermark_at_genesis);
    TEST_RUN(test_claim_is_exclusive_but_still_outstanding);
    TEST_RUN(test_claim_splits_wide_ranges);
    TEST_RUN(test_claim_of_nothing);
    TEST_RUN(test_resolve_partial);
    TEST_RUN(test_release_returns_the_tail);
    TEST_RUN(test_coarsening_never_forgets);
    TEST_RUN(test_rejects_bad_arguments);
    TEST_RUN(test_concurrent_claim_and_resolve);
})
