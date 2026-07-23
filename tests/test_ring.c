/*
 * The overflow path is the reason this ring exists, and no live endpoint will
 * ever exercise it: the demo account delivers well under what the chain
 * produces. So the producer here is synthetic, which also lets the ordering
 * around close and drain be pinned down exactly.
 *
 * Leaks are failures here rather than untidiness: the ring owns every document
 * it holds, including the ones it drops, and the tests run under ASan.
 */
#include "ring.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

/*
 * Stands in for a block notification. Small, but the ring never looks.
 *
 * Deliberately free of assertions: this runs on the producer thread too, and
 * the harness counters are plain globals — asserting here would be a race in
 * the test rather than in the code under test. The literal cannot fail to
 * parse, and test_document_builder checks that once, on one thread.
 */
static idx_json_doc *document(idx_slot slot) {
    char text[64];
    snprintf(text, sizeof(text), "{\"slot\":%llu}", (unsigned long long)slot);

    idx_json_doc *doc = NULL;
    if (idx_json_parse(idx_slice_from_str(text), &doc, NULL) != IDX_OK) {
        return NULL;
    }
    return doc;
}

static uint64_t slot_of(const idx_ring_entry *entry) {
    uint64_t slot = 0;
    idx_json_opt_u64(idx_json_root(entry->doc), "slot", &slot);
    return slot;
}

/* Fills an entry the way the pipeline's receive thread would. */
static idx_status publish(idx_ring *ring, idx_slot slot, size_t bytes,
                          idx_error *err) {
    idx_ring_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.slot = slot;
    entry.doc = document(slot);
    entry.value = idx_json_root(entry.doc);
    entry.tag = 0xABCDEF00u + slot;
    entry.bytes = bytes;
    return idx_ring_publish(ring, &entry, err);
}

static idx_ring *open_ring(size_t depth) {
    idx_ring_options options;
    idx_ring_options_init(&options);
    options.depth = depth;

    idx_ring *ring = NULL;
    idx_error err;
    idx_error_clear(&err);
    TEST_CHECK(idx_ring_new(&options, &ring, &err) == IDX_OK, "new failed: %s",
               err.message);
    return ring;
}

/* The one place the helper itself is checked, on the main thread. */
static void test_document_builder(void) {
    idx_json_doc *doc = document(7);
    TEST_ASSERT(doc != NULL);
    uint64_t slot = 0;
    TEST_ASSERT(idx_json_opt_u64(idx_json_root(doc), "slot", &slot));
    TEST_EQ_UINT(slot, 7u);
    idx_json_free(doc);
}

static void test_publish_consume_order(void) {
    idx_ring *ring = open_ring(4);

    TEST_EQ_INT(publish(ring, 100, 500, NULL), IDX_OK);
    TEST_EQ_INT(publish(ring, 101, 600, NULL), IDX_OK);

    idx_ring_entry entry;
    TEST_EQ_INT(idx_ring_consume(ring, 0, &entry, NULL), IDX_OK);
    TEST_EQ_UINT(entry.seq, 0u);
    TEST_EQ_UINT(entry.slot, 100u);
    TEST_EQ_UINT(entry.bytes, 500u);
    /* The document survived the trip intact, and so did the node the producer
     * picked out of it and the tag it attached. */
    TEST_EQ_UINT(slot_of(&entry), 100u);
    TEST_ASSERT(idx_json_is_object(entry.value));
    TEST_EQ_UINT(idx_json_array_size(idx_json_get(entry.value, "missing")), 0u);
    TEST_EQ_UINT(entry.tag, 0xABCDEF00u + 100u);
    idx_ring_release(ring, &entry);
    TEST_ASSERT(entry.doc == NULL);

    TEST_EQ_INT(idx_ring_consume(ring, 0, &entry, NULL), IDX_OK);
    TEST_EQ_UINT(entry.seq, 1u);
    TEST_EQ_UINT(entry.slot, 101u);
    idx_ring_release(ring, &entry);

    /* Empty and open: a poll with no budget says so rather than blocking. */
    TEST_EQ_INT(idx_ring_consume(ring, 0, &entry, NULL), IDX_ERR_TIMEOUT);

    idx_ring_free(ring);
}

/* The whole point: the producer never waits, the oldest entry is what goes. */
static void test_overflow_drops_the_oldest(void) {
    idx_ring *ring = open_ring(4);

    for (idx_slot slot = 0; slot < 10; slot++) {
        TEST_EQ_INT(publish(ring, slot, 10, NULL),
                    IDX_OK);
    }

    idx_ring_stats stats;
    idx_ring_get_stats(ring, &stats);
    TEST_EQ_UINT(stats.published, 10u);
    TEST_EQ_UINT(stats.dropped, 6u);
    TEST_EQ_UINT(stats.queued, 4u);
    TEST_EQ_UINT(stats.high_water, 4u);
    TEST_EQ_UINT(stats.bytes, 100u);

    /* What survives is the newest depth entries, still in order. */
    for (idx_slot expected = 6; expected < 10; expected++) {
        idx_ring_entry entry;
        TEST_EQ_INT(idx_ring_consume(ring, 0, &entry, NULL), IDX_OK);
        TEST_EQ_UINT(entry.slot, expected);
        /* The sequence is what tells the consumer how much it lost. */
        TEST_EQ_UINT(entry.seq, expected);
        TEST_EQ_UINT(slot_of(&entry), expected);
        idx_ring_release(ring, &entry);
    }

    idx_ring_entry entry;
    TEST_EQ_INT(idx_ring_consume(ring, 0, &entry, NULL), IDX_ERR_TIMEOUT);
    idx_ring_free(ring);
}

/* A consumer holding an entry owns it outright, so nothing the producer does
 * afterwards can invalidate it. */
static void test_held_entry_survives_overflow(void) {
    idx_ring *ring = open_ring(2);

    TEST_EQ_INT(publish(ring, 1, 10, NULL), IDX_OK);

    idx_ring_entry held;
    TEST_EQ_INT(idx_ring_consume(ring, 0, &held, NULL), IDX_OK);
    TEST_EQ_UINT(held.slot, 1u);

    for (idx_slot slot = 2; slot < 8; slot++) {
        TEST_EQ_INT(publish(ring, slot, 10, NULL),
                    IDX_OK);
    }

    TEST_EQ_UINT(slot_of(&held), 1u);
    idx_ring_release(ring, &held);

    idx_ring_entry entry;
    TEST_EQ_INT(idx_ring_consume(ring, 0, &entry, NULL), IDX_OK);
    TEST_EQ_UINT(entry.slot, 6u);
    idx_ring_release(ring, &entry);
    TEST_EQ_INT(idx_ring_consume(ring, 0, &entry, NULL), IDX_OK);
    TEST_EQ_UINT(entry.slot, 7u);
    idx_ring_release(ring, &entry);

    idx_ring_free(ring);
}

/* Sequence numbers never restart, so a gap always means a drop. */
static void test_sequence_exposes_the_gap(void) {
    idx_ring *ring = open_ring(2);

    for (idx_slot slot = 100; slot < 110; slot++) {
        TEST_EQ_INT(publish(ring, slot, 1, NULL),
                    IDX_OK);
    }

    idx_ring_entry first;
    idx_ring_entry second;
    TEST_EQ_INT(idx_ring_consume(ring, 0, &first, NULL), IDX_OK);
    idx_ring_release(ring, &first);
    TEST_EQ_INT(idx_ring_consume(ring, 0, &second, NULL), IDX_OK);
    idx_ring_release(ring, &second);

    /* Consecutive on the way out, and the jump from zero says 8 were lost. */
    TEST_EQ_UINT(first.seq, 8u);
    TEST_EQ_UINT(second.seq - first.seq, 1u);
    TEST_EQ_UINT(second.slot - first.slot, 1u);

    idx_ring_free(ring);
}

static void test_close_drains_then_reports(void) {
    idx_ring *ring = open_ring(4);

    TEST_EQ_INT(publish(ring, 1, 10, NULL), IDX_OK);
    TEST_EQ_INT(publish(ring, 2, 10, NULL), IDX_OK);

    idx_ring_close(ring);
    idx_ring_close(ring); /* idempotent */

    /* Closing refuses new work, and consumes the document rather than leaking
     * it back to a caller that has nowhere to put it. */
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(publish(ring, 3, 10, &err),
                IDX_ERR_CLOSED);
    TEST_ASSERT(err.message[0] != '\0');

    /* What was already queued still comes out. */
    idx_ring_entry entry;
    TEST_EQ_INT(idx_ring_consume(ring, 0, &entry, NULL), IDX_OK);
    TEST_EQ_UINT(entry.slot, 1u);
    idx_ring_release(ring, &entry);

    TEST_EQ_INT(idx_ring_consume(ring, 0, &entry, NULL), IDX_OK);
    TEST_EQ_UINT(entry.slot, 2u);
    idx_ring_release(ring, &entry);

    /* Drained, so now it reports the close rather than a timeout. */
    TEST_EQ_INT(idx_ring_consume(ring, 1000, &entry, NULL), IDX_ERR_CLOSED);

    idx_ring_free(ring);
}

/* Freeing a ring with entries still queued must not leak them. */
static void test_free_releases_queued_documents(void) {
    idx_ring *ring = open_ring(4);
    for (idx_slot slot = 0; slot < 3; slot++) {
        TEST_EQ_INT(publish(ring, slot, 10, NULL),
                    IDX_OK);
    }
    idx_ring_free(ring);
}

static void test_rejects_bad_arguments(void) {
    idx_error err;
    idx_error_clear(&err);

    idx_ring *ring = NULL;
    TEST_EQ_INT(idx_ring_new(NULL, NULL, &err), IDX_ERR_INVALID_ARG);

    idx_ring_options options;
    idx_ring_options_init(&options);
    options.depth = 1;
    TEST_EQ_INT(idx_ring_new(&options, &ring, &err), IDX_ERR_RANGE);

    /* Publishing into nothing still consumes the document. */
    idx_ring_entry orphan;
    memset(&orphan, 0, sizeof(orphan));
    orphan.doc = document(1);
    TEST_EQ_INT(idx_ring_publish(NULL, &orphan, &err), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_ring_publish(ring, NULL, &err), IDX_ERR_INVALID_ARG);

    idx_ring_entry entry;
    TEST_EQ_INT(idx_ring_consume(NULL, 0, &entry, &err), IDX_ERR_INVALID_ARG);

    /* Every teardown path tolerates NULL. */
    idx_ring_free(NULL);
    idx_ring_close(NULL);
    idx_ring_release(NULL, NULL);
    idx_ring_get_stats(NULL, NULL);
}

static void test_defaults(void) {
    idx_ring_options options;
    memset(&options, 0xAA, sizeof(options));
    idx_ring_options_init(&options);
    TEST_EQ_UINT(options.depth, IDX_RING_DEFAULT_DEPTH);

    /* A zeroed field selects the default rather than failing. */
    memset(&options, 0, sizeof(options));
    idx_ring *ring = NULL;
    TEST_EQ_INT(idx_ring_new(&options, &ring, NULL), IDX_OK);
    idx_ring_free(ring);

    /* So does no options struct at all. */
    ring = NULL;
    TEST_EQ_INT(idx_ring_new(NULL, &ring, NULL), IDX_OK);
    idx_ring_free(ring);

    idx_ring_options_init(NULL); /* must not crash */
}

/* ------------------------------------------------------------- concurrency -- */

typedef struct {
    idx_ring *ring;
    idx_slot count;
} producer_args;

static void *produce(void *argument) {
    producer_args *args = (producer_args *)argument;
    for (idx_slot slot = 0; slot < args->count; slot++) {
        if (publish(args->ring, slot, 64, NULL) !=
            IDX_OK) {
            break;
        }
    }
    idx_ring_close(args->ring);
    return NULL;
}

/*
 * A producer that never waits against a consumer slow enough to be overrun.
 * The assertions are the invariants that must hold whatever the interleaving;
 * the sanitizers are what check the rest.
 */
static void test_concurrent_producer_and_consumer(void) {
    idx_ring *ring = open_ring(4);

    producer_args args;
    args.ring = ring;
    args.count = 3000;

    pthread_t producer;
    TEST_EQ_INT(pthread_create(&producer, NULL, produce, &args), 0);

    uint64_t consumed = 0;
    uint64_t last_seq = 0;
    idx_slot last_slot = 0;
    bool first = true;

    for (;;) {
        idx_ring_entry entry;
        idx_status status = idx_ring_consume(ring, 200, &entry, NULL);
        if (status == IDX_ERR_CLOSED) {
            break;
        }
        if (status != IDX_OK) {
            continue;
        }

        /* Sequences and slots only move forward, however much was lost, and
         * the document always matches the slot it was published under. */
        if (!first) {
            TEST_ASSERT(entry.seq > last_seq);
            TEST_ASSERT(entry.slot > last_slot);
        }
        TEST_EQ_UINT(slot_of(&entry), entry.slot);
        last_seq = entry.seq;
        last_slot = entry.slot;
        first = false;
        consumed++;

        idx_ring_release(ring, &entry);
    }

    TEST_EQ_INT(pthread_join(producer, NULL), 0);

    idx_ring_stats stats;
    idx_ring_get_stats(ring, &stats);
    TEST_EQ_UINT(stats.published, 3000u);
    TEST_EQ_UINT(stats.consumed, consumed);
    /* Nothing is invented and nothing vanishes unaccounted for. */
    TEST_EQ_UINT(stats.consumed + stats.dropped, stats.published);
    TEST_EQ_UINT(stats.queued, 0u);

    idx_ring_free(ring);
}

TEST_MAIN({
    TEST_RUN(test_document_builder);
    TEST_RUN(test_publish_consume_order);
    TEST_RUN(test_overflow_drops_the_oldest);
    TEST_RUN(test_held_entry_survives_overflow);
    TEST_RUN(test_sequence_exposes_the_gap);
    TEST_RUN(test_close_drains_then_reports);
    TEST_RUN(test_free_releases_queued_documents);
    TEST_RUN(test_rejects_bad_arguments);
    TEST_RUN(test_defaults);
    TEST_RUN(test_concurrent_producer_and_consumer);
})
