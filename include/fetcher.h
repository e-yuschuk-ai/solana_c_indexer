/*
 * The recovery path: worker threads that turn outstanding gaps back into
 * blocks (ROADMAP.md milestone M4).
 *
 * Each worker claims a range from idx_gaps, asks `getBlocks` which slots in it
 * the chain actually produced, fetches those over HTTP and publishes them.
 * Slots the chain skipped, and slots the endpoint no longer retains, are
 * resolved on the spot: there is nothing to fetch and nothing to wait for.
 *
 * A worker owns its own idx_rpc, because a client is one connection and is not
 * thread-safe, and because that is what keeps connection reuse working per
 * worker (decision D1).
 *
 * What a worker does not do is resolve the gap for a block it fetched. It
 * resolves only the slots that will never produce one. A fetched block is
 * resolved by whoever commits it, so a crash between fetching and committing
 * leaves the slot outstanding and it is fetched again — which is the safe
 * direction, since re-indexing a slot is idempotent and losing one is not.
 *
 * The output ring must be a blocking one. Dropping a recovered block would put
 * its slot straight back into the gap set to be fetched again, forever;
 * stalling a fetcher costs nothing, unlike stalling the socket (decision D6).
 */
#ifndef IDX_FETCHER_H
#define IDX_FETCHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "error.h"
#include "gaps.h"
#include "ring.h"
#include "types.h"

typedef struct idx_fetcher_pool idx_fetcher_pool;

/* Slots one worker claims at a time when the caller does not choose. */
#define IDX_FETCHER_DEFAULT_CLAIM_SPAN 64u

typedef struct {
    /* Required, and borrowed: all three must outlive the pool. */
    const idx_config *config; /* rpc_url, commitment, tx_details, concurrency */
    idx_gaps *gaps;
    idx_ring *output; /* must have been created with block_when_full */

    /*
     * Slots per claim, and the width of the getBlocks range that follows from
     * it. Narrow keeps workers sharing one wide hole; wide cuts the number of
     * round trips. This is a starting point rather than a setting: a provider
     * that caps the range answers a wider request with a rejection, and the
     * pool halves its claims until they are accepted. 0 selects the default.
     */
    uint64_t claim_span;

    /* How long a worker waits before looking for work again when there is
     * none. 0 selects 250 ms. */
    int idle_ms;
} idx_fetcher_options;

void idx_fetcher_options_init(idx_fetcher_options *options);

typedef struct {
    uint64_t ranges_claimed;
    uint64_t blocks_fetched;  /* published downstream */
    uint64_t slots_absent;    /* skipped by the chain, or no longer retained */
    uint64_t fetch_failures;  /* released for another attempt */
    uint64_t ranges_abandoned;/* the endpoint refused them; retrying will not help */
    uint64_t span_narrowings; /* times the claim width was halved */
    size_t workers;
} idx_fetcher_stats;

/*
 * Starts `config->concurrency` workers. They begin looking for work
 * immediately; an empty gap set simply means they idle.
 */
idx_status idx_fetcher_pool_start(const idx_fetcher_options *options,
                                  idx_fetcher_pool **out, idx_error *err);

/*
 * Asks every worker to stop, waits for them, and frees the pool. A worker
 * stops between blocks, so a fetch in flight completes first and whatever it
 * could not finish goes back to the gap set. Safe with NULL.
 */
void idx_fetcher_pool_stop(idx_fetcher_pool *pool);

void idx_fetcher_pool_get_stats(const idx_fetcher_pool *pool,
                                idx_fetcher_stats *out);

#endif /* IDX_FETCHER_H */
