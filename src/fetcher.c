#include "fetcher.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "rpc.h"
#include "vec.h"

#define IDX_FETCHER_DEFAULT_IDLE_MS 250

typedef struct {
    idx_fetcher_pool *pool;
    pthread_t thread;
    idx_rpc *rpc; /* one connection per worker (decision D1) */
    size_t index;
} idx_fetcher_worker;

struct idx_fetcher_pool {
    idx_fetcher_options options;

    idx_fetcher_worker *workers;
    size_t worker_count;

    /* Guards `stop`, the idle wait, and the shared statistics. */
    pthread_mutex_t lock;
    pthread_cond_t wakeup;
    bool stop;

    /*
     * How many slots a claim covers right now. Starts at the configured span
     * and narrows when the endpoint rejects the width: providers cap the
     * getBlocks range per plan — five slots on some free tiers — and answer a
     * wider request with a rejection rather than a partial result. Halving and
     * retrying finds the cap without being told it.
     */
    uint64_t span;

    idx_fetcher_stats stats;
};

void idx_fetcher_options_init(idx_fetcher_options *options) {
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->claim_span = IDX_FETCHER_DEFAULT_CLAIM_SPAN;
    options->idle_ms = IDX_FETCHER_DEFAULT_IDLE_MS;
}

static uint64_t current_span(idx_fetcher_pool *pool) {
    pthread_mutex_lock(&pool->lock);
    uint64_t span = pool->span;
    pthread_mutex_unlock(&pool->lock);
    return span;
}

/*
 * Halves the claim width after the endpoint refused a range `failed_width`
 * wide.
 *
 * Only the first worker to report a given width narrows. Several are usually
 * in flight at once and all fail together, and letting each one halve turns a
 * single rejection into one narrowing per worker — which is how a five-slot cap
 * ends up driving the width down to one.
 */
static void narrow_span(idx_fetcher_pool *pool, uint64_t failed_width) {
    pthread_mutex_lock(&pool->lock);
    /*
     * A claim is only as wide as the hole it came from, so its width is not
     * necessarily the span. Comparing against the width that actually failed
     * is what converges: a narrower span than that means another worker has
     * already reacted to this width or worse, and halving again would
     * overshoot.
     */
    if (failed_width <= 1 || pool->span < failed_width) {
        pthread_mutex_unlock(&pool->lock);
        return;
    }
    pool->span = failed_width / 2;
    uint64_t span = pool->span;
    pool->stats.span_narrowings++;
    pthread_mutex_unlock(&pool->lock);

    IDX_INFO("gap recovery: narrowing claims to %llu slots; the endpoint "
             "refused a %llu slot getBlocks range",
             (unsigned long long)span, (unsigned long long)failed_width);
}

static bool stopping(idx_fetcher_pool *pool) {
    pthread_mutex_lock(&pool->lock);
    bool stop = pool->stop;
    pthread_mutex_unlock(&pool->lock);
    return stop;
}

/* Waits up to `ms`, returning early when the pool is asked to stop. */
static void idle(idx_fetcher_pool *pool, int ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&pool->lock);
    if (!pool->stop) {
        pthread_cond_timedwait(&pool->wakeup, &pool->lock, &deadline);
    }
    pthread_mutex_unlock(&pool->lock);
}

/* Statistics are shared, so they are counted under the pool's lock rather than
 * per worker. This happens once per block, not once per byte. */
static void bump(idx_fetcher_pool *pool, uint64_t *field, uint64_t by) {
    pthread_mutex_lock(&pool->lock);
    *field += by;
    pthread_mutex_unlock(&pool->lock);
}

/*
 * Hands one recovered block downstream. Ownership of the document moves to the
 * ring, including on failure, so there is no path here that leaks it.
 */
static idx_status publish_block(idx_fetcher_pool *pool, idx_slot slot,
                                idx_rpc_response *response, idx_error *err) {
    idx_ring_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.slot = slot;
    entry.doc = response->doc;
    entry.value = response->result;
    /* No tag: what identifies these is the ring they arrive on. Only the
     * recovery path publishes here. */
    entry.bytes = 0; /* the RPC client accounts for its own transfer */

    response->doc = NULL;
    idx_rpc_response_free(response);

    return idx_ring_publish(pool->options.output, &entry, err);
}

/*
 * Works one claimed range.
 *
 * getBlocks is what makes this tractable: it says which slots in the range the
 * chain actually produced, so the ones it omits are resolved without a fetch
 * each. Only the slots that exist cost a round trip.
 */
static void work_range(idx_fetcher_worker *worker, idx_gap_range range) {
    idx_fetcher_pool *pool = worker->pool;
    const idx_config *cfg = pool->options.config;

    idx_vec slots;
    idx_vec_init(&slots, sizeof(uint64_t));

    idx_error err;
    idx_error_clear(&err);
    idx_status status = idx_rpc_get_blocks(worker->rpc, range.from, range.to,
                                           cfg->commitment, &slots, &err);

    if (status == IDX_ERR_NOT_FOUND) {
        /* The endpoint no longer retains this stretch. Nothing will ever come
         * of it, so it stops being outstanding. */
        IDX_DEBUG("gap %llu..%llu is no longer retained",
                  (unsigned long long)range.from, (unsigned long long)range.to);
        idx_gaps_resolve(pool->options.gaps, range.from, range.to);
        bump(pool, &pool->stats.slots_absent, range.to - range.from + 1);
        idx_vec_free(&slots);
        return;
    }
    if (status == IDX_ERR_REMOTE) {
        /*
         * The endpoint rejected the request rather than failed to serve it.
         * The usual reason is the width — plans cap the getBlocks range — so
         * anything wider than a single slot goes back to be retried at
         * whatever width is being accepted by then. Abandoning it here would
         * throw away slots that are perfectly fetchable, and advance the
         * cursor past them.
         */
        idx_slot width = range.to - range.from + 1;
        if (width > 1) {
            narrow_span(pool, width);
            idx_gaps_release(pool->options.gaps, range.from, range.to, NULL);
            idx_vec_free(&slots);
            return;
        }

        /*
         * One slot, and still refused: the width was never the problem, and
         * rpc.h is clear that retrying a rejection does not help. This is the
         * only path that gives up on a slot, so it is loud.
         */
        IDX_WARN("gap %llu abandoned: %s", (unsigned long long)range.from,
                 err.message);
        idx_gaps_resolve(pool->options.gaps, range.from, range.to);
        bump(pool, &pool->stats.ranges_abandoned, 1);
        idx_vec_free(&slots);
        return;
    }
    if (status != IDX_OK) {
        IDX_DEBUG("getBlocks(%llu..%llu): %s", (unsigned long long)range.from,
                  (unsigned long long)range.to, err.message);
        idx_gaps_release(pool->options.gaps, range.from, range.to, NULL);
        bump(pool, &pool->stats.fetch_failures, 1);
        idx_vec_free(&slots);
        return;
    }

    idx_rpc_block_options block_options;
    idx_rpc_block_options_init(&block_options);
    block_options.commitment = cfg->commitment;
    block_options.transaction_details = idx_tx_details_name(cfg->tx_details);

    /* Everything below `cursor` in the range has been dealt with. */
    idx_slot cursor = range.from;

    for (size_t i = 0; i < idx_vec_len(&slots); i++) {
        if (stopping(pool)) {
            break;
        }

        idx_slot slot = *(uint64_t *)idx_vec_at(&slots, i);
        if (slot < cursor || slot > range.to) {
            continue; /* outside what we claimed; not ours to resolve */
        }

        /* The slots getBlocks skipped over never existed. */
        if (slot > cursor) {
            idx_gaps_resolve(pool->options.gaps, cursor, slot - 1);
            bump(pool, &pool->stats.slots_absent, slot - cursor);
        }

        idx_rpc_response response;
        idx_error fetch_err;
        idx_error_clear(&fetch_err);
        idx_status fetched =
            idx_rpc_get_block(worker->rpc, slot, &block_options, &response,
                              &fetch_err);

        if (fetched == IDX_ERR_NOT_FOUND) {
            /* Between the enumeration and now it was pruned, or it was never
             * really there. Either way there is nothing to index. */
            idx_gaps_resolve(pool->options.gaps, slot, slot);
            bump(pool, &pool->stats.slots_absent, 1);
            cursor = slot + 1;
            continue;
        }
        if (fetched != IDX_OK) {
            /* Stop here and give the rest back, so another attempt picks up
             * exactly what was not done. */
            IDX_DEBUG("getBlock(%llu): %s", (unsigned long long)slot,
                      fetch_err.message);
            idx_gaps_release(pool->options.gaps, slot, range.to, NULL);
            bump(pool, &pool->stats.fetch_failures, 1);
            idx_vec_free(&slots);
            return;
        }

        idx_error publish_err;
        idx_error_clear(&publish_err);
        if (publish_block(pool, slot, &response, &publish_err) != IDX_OK) {
            /* The ring closed: the pipeline is shutting down. What is left
             * goes back so a later run finds it. */
            idx_gaps_release(pool->options.gaps, slot, range.to, NULL);
            idx_vec_free(&slots);
            return;
        }

        /*
         * Deliberately not resolved here. The slot stops being outstanding
         * when it is committed, not when it is published, so a crash in
         * between costs a refetch instead of the block.
         */
        bump(pool, &pool->stats.blocks_fetched, 1);
        cursor = slot + 1;
    }

    /* Whatever the enumeration left past the last block never existed. */
    if (cursor <= range.to) {
        idx_gaps_resolve(pool->options.gaps, cursor, range.to);
        bump(pool, &pool->stats.slots_absent, range.to - cursor + 1);
    }

    idx_vec_free(&slots);
}

static void *run_worker(void *argument) {
    idx_fetcher_worker *worker = (idx_fetcher_worker *)argument;
    idx_fetcher_pool *pool = worker->pool;

    while (!stopping(pool)) {
        idx_gap_range range;
        if (!idx_gaps_claim(pool->options.gaps, current_span(pool), &range)) {
            idle(pool, pool->options.idle_ms);
            continue;
        }

        bump(pool, &pool->stats.ranges_claimed, 1);
        work_range(worker, range);
    }
    return NULL;
}

idx_status idx_fetcher_pool_start(const idx_fetcher_options *options,
                                  idx_fetcher_pool **out, idx_error *err) {
    if (options == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null argument");
    }
    if (options->config == NULL || options->gaps == NULL ||
        options->output == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "fetcher needs a config, a gap set and an output ring");
    }
    if (options->config->rpc_url[0] == '\0') {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "no rpc endpoint to recover gaps from");
    }

    size_t count_wanted = options->config->concurrency;
    if (count_wanted == 0) {
        count_wanted = 1;
    }

    idx_fetcher_pool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "out of memory");
    }
    pool->options = *options;
    if (pool->options.claim_span == 0) {
        pool->options.claim_span = IDX_FETCHER_DEFAULT_CLAIM_SPAN;
    }
    if (pool->options.idle_ms <= 0) {
        pool->options.idle_ms = IDX_FETCHER_DEFAULT_IDLE_MS;
    }
    pool->span = pool->options.claim_span;

    pool->workers = calloc(count_wanted, sizeof(*pool->workers));
    if (pool->workers == NULL) {
        free(pool);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "out of memory");
    }

    pthread_mutex_init(&pool->lock, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&pool->wakeup, &attr);
    pthread_condattr_destroy(&attr);

    const char *urls[1] = {options->config->rpc_url};
    idx_rpc_options rpc_options;
    idx_rpc_options_init(&rpc_options);
    rpc_options.urls = urls;
    rpc_options.url_count = 1;
    rpc_options.blocks_range_limit = options->config->blocks_range_limit;

    /*
     * Signals belong to the thread that installed the handler, so the workers
     * inherit a mask that blocks them.
     */
    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);

    idx_status status = IDX_OK;
    for (size_t i = 0; i < count_wanted; i++) {
        idx_fetcher_worker *worker = &pool->workers[i];
        worker->pool = pool;
        worker->index = i;

        status = idx_rpc_open(&rpc_options, &worker->rpc, err);
        if (status != IDX_OK) {
            break;
        }
        if (pthread_create(&worker->thread, NULL, run_worker, worker) != 0) {
            idx_rpc_close(worker->rpc);
            worker->rpc = NULL;
            status = IDX_FAIL(err, IDX_ERR_INTERNAL,
                              "cannot start fetcher thread %zu", i);
            break;
        }
        pool->worker_count++;
    }

    pthread_sigmask(SIG_SETMASK, &previous, NULL);

    if (status != IDX_OK) {
        /* Whatever did start is stopped, so a partial start leaves nothing
         * running behind it. */
        idx_fetcher_pool_stop(pool);
        return status;
    }

    pool->stats.workers = pool->worker_count;
    IDX_INFO("gap recovery: %zu fetchers, %llu slots per claim",
             pool->worker_count, (unsigned long long)pool->options.claim_span);

    *out = pool;
    return IDX_OK;
}

void idx_fetcher_pool_stop(idx_fetcher_pool *pool) {
    if (pool == NULL) {
        return;
    }

    pthread_mutex_lock(&pool->lock);
    pool->stop = true;
    pthread_cond_broadcast(&pool->wakeup);
    pthread_mutex_unlock(&pool->lock);

    for (size_t i = 0; i < pool->worker_count; i++) {
        pthread_join(pool->workers[i].thread, NULL);
    }
    for (size_t i = 0; i < pool->worker_count; i++) {
        idx_rpc_close(pool->workers[i].rpc);
    }

    pthread_cond_destroy(&pool->wakeup);
    pthread_mutex_destroy(&pool->lock);
    free(pool->workers);
    free(pool);
}

void idx_fetcher_pool_get_stats(const idx_fetcher_pool *pool,
                                idx_fetcher_stats *out) {
    if (pool == NULL || out == NULL) {
        return;
    }
    idx_fetcher_pool *unconst = (idx_fetcher_pool *)pool;
    pthread_mutex_lock(&unconst->lock);
    *out = pool->stats;
    pthread_mutex_unlock(&unconst->lock);
}
