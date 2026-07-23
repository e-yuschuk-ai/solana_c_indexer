#include "pipeline.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fetcher.h"
#include "gaps.h"
#include "log.h"
#include "pubsub.h"
#include "ring.h"
#include "rpc.h"

/*
 * How long the receive loop blocks before looking at the stop flag. Noticing a
 * dead connection is the pubsub layer's job, not this one's, so the only thing
 * this bounds is how long a shutdown waits — and one idle poll per second
 * costs nothing next to 12 MiB/s of blocks.
 */
#define IDX_PIPELINE_POLL_TIMEOUT_MS 1000
#define IDX_PIPELINE_SAVE_INTERVAL_MS 1000

/* Room for the filter — a base58 pubkey at most — plus the options object. */
#define IDX_PIPELINE_PARAMS_MAX (IDX_CONFIG_STR_MAX + 128)

struct idx_pipeline {
    idx_pipeline_options options;
    char subscribe_params[IDX_PIPELINE_PARAMS_MAX];

    /* The hand-off. The receive thread publishes, the processing thread
     * consumes; neither touches the other's state (decision D6). */
    idx_ring *ring;

    /*
     * The recovery side. Holes go into `gaps`, the fetchers turn them back
     * into blocks, and those arrive on `recovered` — a blocking ring, because
     * a dropped recovered block would become a gap again and be refetched
     * forever.
     */
    idx_gaps *gaps;
    idx_ring *recovered;
    idx_fetcher_pool *fetchers;

    /* The pool is stopped and freed before the caller reads its statistics,
     * so the last reading is kept here rather than lost with it. */
    idx_fetcher_stats fetcher_stats;
    pthread_t processor;
    bool processor_running;

    /*
     * Written by idx_pipeline_request_stop, which may be a signal handler, and
     * read by the receive loop.
     */
    volatile sig_atomic_t stop;

    /* Set by the processing thread when it gives up, so the receive loop stops
     * feeding a pipeline that is going nowhere. Its status and message are only
     * read after the join. */
    volatile sig_atomic_t processor_failed;
    idx_status processor_status;
    idx_error processor_err;

    /*
     * Lowest slot worth indexing, 0 for "wherever the tip is". Fixed before the
     * threads start, read-only afterwards.
     */
    idx_slot floor;

    /*
     * One struct, but each field has exactly one writer: the receive thread
     * owns bytes, reconnects, slots_skipped and used_fallback, the processing
     * thread owns the rest. A reader may therefore catch a slightly mixed
     * snapshot, which is what monitoring counters are.
     */
    idx_pipeline_stats stats;

    /* Processing thread only. */
    idx_arena arena;
    double last_save;
    bool save_warned;
    uint64_t next_seq;         /* the live ring sequence expected next */
    idx_slot highest_committed; /* IDX_SLOT_NONE until the first commit */
};

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void sleep_ms(int milliseconds) {
    struct timespec request;
    request.tv_sec = milliseconds / 1000;
    request.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&request, NULL);
}

const char *idx_block_origin_name(idx_block_origin origin) {
    switch (origin) {
    case IDX_BLOCK_FROM_SUBSCRIPTION:
        return "blockSubscribe";
    case IDX_BLOCK_FROM_RPC:
        return "getBlock";
    }
    return "unknown";
}

/* ------------------------------------------------------- notification reads -- */

idx_status idx_pipeline_read_block_notification(idx_json_val result,
                                                idx_slot *slot,
                                                idx_json_val *block,
                                                idx_error *err) {
    if (slot == NULL || block == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null output parameter");
    }
    if (!idx_json_is_object(result)) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "blockNotification result is %s, expected an object",
                        idx_json_type_name(result));
    }

    idx_json_val value = idx_json_get(result, "value");

    /*
     * context.slot is where the slot belongs; value.slot repeats it, and is
     * the only source left if a provider omits the context.
     */
    uint64_t number = 0;
    if (!idx_json_opt_u64(idx_json_get(result, "context"), "slot", &number) &&
        !idx_json_opt_u64(value, "slot", &number)) {
        return IDX_FAIL(err, IDX_ERR_PARSE, "blockNotification carries no slot");
    }
    *slot = number;

    if (!idx_json_is_object(value)) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "blockNotification value is %s, expected an object",
                        idx_json_type_name(value));
    }

    idx_json_val failure = idx_json_get(value, "err");
    if (idx_json_is_present(failure) && !idx_json_is_null(failure)) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND,
                        "slot %llu was notified with an error",
                        (unsigned long long)number);
    }

    idx_json_val candidate = idx_json_get(value, "block");
    if (!idx_json_is_object(candidate)) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "slot %llu carries no block",
                        (unsigned long long)number);
    }

    *block = candidate;
    return IDX_OK;
}

idx_status idx_pipeline_read_slot_notification(idx_json_val result,
                                               idx_slot *slot, idx_error *err) {
    if (slot == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null output parameter");
    }
    if (!idx_json_is_object(result)) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "slotNotification result is %s, expected an object",
                        idx_json_type_name(result));
    }

    uint64_t number = 0;
    if (!idx_json_opt_u64(result, "slot", &number)) {
        return IDX_FAIL(err, IDX_ERR_PARSE, "slotNotification carries no slot");
    }
    *slot = number;
    return IDX_OK;
}

/* --------------------------------------------------- shared, read-only state -- */

static bool stop_requested(const idx_pipeline *pipeline) {
    return pipeline->stop != 0 || pipeline->processor_failed != 0;
}

static bool above_end(const idx_pipeline *pipeline, idx_slot slot) {
    idx_slot end = pipeline->options.config->end_slot;
    return end != 0 && slot > end;
}

static bool at_or_above_end(const idx_pipeline *pipeline, idx_slot slot) {
    idx_slot end = pipeline->options.config->end_slot;
    return end != 0 && slot >= end;
}

static bool below_floor(const idx_pipeline *pipeline, idx_slot slot) {
    return pipeline->floor != 0 && slot < pipeline->floor;
}

/* --------------------------------------------------- the processing thread -- */

/*
 * Records that a live slot reached the pipeline, and turns any distance from
 * the last one into a hole for the fetchers.
 *
 * The cause does not matter here and deliberately is not distinguished: a
 * reconnect the socket could not replay and a block the ring dropped under
 * pressure both show up as the same discontinuity, and both are recovered the
 * same way.
 */
static void observe_live_slot(idx_pipeline *pipeline, idx_slot slot) {
    idx_slot_cursor *cursor = pipeline->options.cursor;
    idx_slot previous = cursor->last_seen;

    if (previous != IDX_SLOT_NONE && slot > previous + 1) {
        idx_slot from = previous + 1;
        idx_slot to = slot - 1;
        pipeline->stats.slots_missed += to - from + 1;

        idx_error gap_err;
        idx_error_clear(&gap_err);
        if (idx_gaps_add(pipeline->gaps, from, to, &gap_err) != IDX_OK) {
            /* Losing a hole means losing the slots in it, so this is louder
             * than the miss itself. */
            IDX_ERROR("cannot record the gap %llu..%llu: %s",
                      (unsigned long long)from, (unsigned long long)to,
                      gap_err.message);
        } else {
            IDX_DEBUG("gap: slots %llu..%llu queued for recovery",
                      (unsigned long long)from, (unsigned long long)to);
        }
    }
    idx_slot_cursor_observe(cursor, slot);
}

/*
 * Moves the durable frontier to wherever there is nothing outstanding beneath
 * it. Assigned rather than advanced: a hole found below the current position
 * has to pull it back, or a restart resumes past slots that were never
 * indexed.
 */
static void update_watermark(idx_pipeline *pipeline) {
    idx_slot watermark =
        idx_gaps_watermark(pipeline->gaps, pipeline->highest_committed);
    idx_slot_cursor_set_indexed(pipeline->options.cursor, watermark);
    pipeline->stats.last_indexed = watermark;
}

static void save_cursor(idx_pipeline *pipeline, bool force) {
    const idx_slot_cursor *cursor = pipeline->options.cursor;
    if (cursor->path[0] == '\0') {
        return;
    }

    double now = monotonic_seconds();
    if (!force && pipeline->options.save_interval_ms > 0) {
        double elapsed_ms = (now - pipeline->last_save) * 1000.0;
        if (elapsed_ms < (double)pipeline->options.save_interval_ms) {
            return;
        }
    }

    idx_error save_err;
    idx_error_clear(&save_err);
    if (idx_slot_cursor_save(cursor, &save_err) != IDX_OK) {
        pipeline->stats.save_failures++;
        /*
         * Degraded, not fatal: indexing continues and only a restart pays for
         * it, by resuming further back. Worth saying once, not every second.
         */
        if (!pipeline->save_warned) {
            IDX_WARN("cannot persist the slot cursor: %s", save_err.message);
            pipeline->save_warned = true;
        } else {
            IDX_DEBUG("cannot persist the slot cursor: %s", save_err.message);
        }
        return;
    }
    pipeline->last_save = now;

    /* The forced save is the last write before the process exits, so it is
     * the one an operator most needs to see; periodic saves during a run
     * would be one line per second and belong at DEBUG instead. */
    idx_log_level level = force ? IDX_LOG_INFO : IDX_LOG_DEBUG;
    if (cursor->last_indexed != IDX_SLOT_NONE) {
        IDX_LOG(level, "cursor: persisted to %s, last indexed slot %llu",
                cursor->path, (unsigned long long)cursor->last_indexed);
    } else {
        IDX_LOG(level, "cursor: persisted to %s, nothing indexed yet",
                cursor->path);
    }
}

/*
 * Hands the block to the consumer and, if it takes it, advances the indexed
 * frontier. The arena is reset as soon as the handler returns, so anything the
 * handler allocated from it is gone by the time this returns.
 */
static idx_status commit_block(idx_pipeline *pipeline,
                               const idx_raw_block *block, idx_error *err) {
    idx_status status =
        pipeline->options.handler(block, pipeline->options.user, err);
    idx_arena_reset(&pipeline->arena);

    if (status != IDX_OK) {
        pipeline->stats.handler_errors++;
        return status;
    }

    pipeline->stats.blocks++;
    if (pipeline->highest_committed == IDX_SLOT_NONE ||
        block->slot > pipeline->highest_committed) {
        pipeline->highest_committed = block->slot;
    }

    /*
     * Resolved here rather than by the fetcher that produced it: the slot stops
     * being outstanding when it is committed, not when it is fetched, so a
     * crash in between costs a refetch instead of the block.
     */
    idx_gaps_resolve(pipeline->gaps, block->slot, block->slot);
    update_watermark(pipeline);

    save_cursor(pipeline, false);
    return IDX_OK;
}

/*
 * The other half of the pipeline. Owns the slot cursor and the arena, and
 * never touches the socket; everything it sees arrived through the ring.
 */
/* Commits one entry, whichever side it arrived from. */
static idx_status handle_entry(idx_pipeline *pipeline, idx_ring_entry *entry,
                               idx_ring *from, idx_block_origin origin) {
    idx_raw_block block;
    block.slot = entry->slot;
    block.value = entry->value;
    block.bytes = entry->bytes;
    block.origin = origin;
    block.arena = &pipeline->arena;

    idx_status status = commit_block(pipeline, &block, &pipeline->processor_err);
    idx_ring_release(from, entry);
    return status;
}

/*
 * The other half of the pipeline. Owns the slot cursor and the arena, and
 * never touches the socket; everything it sees arrived through a ring.
 *
 * It drains two of them. The live ring carries the tip and drives gap
 * detection, so it is polled first and its slots set the frontier. The
 * recovery ring carries blocks the fetchers went back for; those are below the
 * frontier by definition and must not be mistaken for the stream jumping
 * backwards, so they only commit and resolve.
 */
static void *run_processor(void *argument) {
    idx_pipeline *pipeline = (idx_pipeline *)argument;
    idx_status status = IDX_OK;

    bool live_done = false;
    bool recovery_done = false;

    while (!live_done || !recovery_done) {
        idx_ring_entry entry;

        if (!live_done) {
            /* A short wait rather than a long one, because the recovery ring
             * is checked in the same pass and neither should sit behind the
             * other's timeout. */
            idx_status code = idx_ring_consume(pipeline->ring, 50, &entry, NULL);
            if (code == IDX_ERR_CLOSED) {
                live_done = true;
            } else if (code == IDX_OK) {
                /*
                 * The sequence is dense at the producer, so a jump is the ring
                 * having dropped entries under pressure. What those slots were
                 * is not recorded anywhere — but the slot discontinuity says
                 * it just as well, and observe_live_slot turns it into a gap.
                 */
                if (entry.seq != pipeline->next_seq) {
                    pipeline->stats.queue_dropped +=
                        entry.seq - pipeline->next_seq;
                }
                pipeline->next_seq = entry.seq + 1;

                observe_live_slot(pipeline, entry.slot);
                status = handle_entry(pipeline, &entry, pipeline->ring,
                                      IDX_BLOCK_FROM_SUBSCRIPTION);
                if (status != IDX_OK) {
                    break;
                }
                continue;
            }
        }

        if (!recovery_done) {
            /* Once the tip is done this is the only source left, so it is
             * worth waiting on properly. */
            int timeout = live_done ? 200 : 0;
            idx_status code =
                idx_ring_consume(pipeline->recovered, timeout, &entry, NULL);
            if (code == IDX_ERR_CLOSED) {
                recovery_done = true;
            } else if (code == IDX_OK) {
                pipeline->stats.blocks_recovered++;
                status = handle_entry(pipeline, &entry, pipeline->recovered,
                                      IDX_BLOCK_FROM_RPC);
                if (status != IDX_OK) {
                    break;
                }
            }
        }
    }

    if (status != IDX_OK) {
        pipeline->processor_status = status;
        /* Tell the receive loop before it publishes anything else, and make
         * the next publish on either side fail rather than pile up work
         * nobody will take. A fetcher waiting for room is released by this
         * too, which is what keeps the shutdown from wedging. */
        pipeline->processor_failed = 1;
        idx_ring_close(pipeline->ring);
        idx_ring_close(pipeline->recovered);
    }

    /* The cursor is this thread's, so this is the only place it can be
     * persisted for the last time. */
    save_cursor(pipeline, true);
    return NULL;
}

static idx_status start_processor(idx_pipeline *pipeline, idx_error *err) {
    /*
     * Signals belong to the thread that installed the handler. Blocking them
     * here means a SIGINT always interrupts the receive loop's poll rather
     * than landing on a thread that cannot act on it.
     */
    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);

    int failure = pthread_create(&pipeline->processor, NULL, run_processor,
                                 pipeline);

    pthread_sigmask(SIG_SETMASK, &previous, NULL);

    if (failure != 0) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL,
                        "cannot start the processing thread");
    }
    pipeline->processor_running = true;
    return IDX_OK;
}

/* ------------------------------------------------------ the receive thread -- */

static void note_reconnects(idx_pipeline *pipeline, const idx_pubsub *pubsub) {
    idx_pubsub_stats stats;
    idx_pubsub_get_stats(pubsub, &stats);
    if (stats.reconnects <= pipeline->stats.reconnects) {
        return;
    }
    pipeline->stats.reconnects = stats.reconnects;
    /* Which slots were missed is the cursor's to say, and the cursor belongs
     * to the other thread; it sees the hole when the stream resumes. */
    IDX_WARN("reconnected; the slots missed while down need replaying");
}

static idx_status open_subscription(idx_pipeline *pipeline, const char *method,
                                    const char *unsubscribe_method,
                                    const char *params, idx_pubsub **out,
                                    uint64_t *handle, idx_error *err) {
    idx_pubsub_options options;
    idx_pubsub_options_init(&options);
    options.url = pipeline->options.config->wss_url;

    idx_pubsub *pubsub = NULL;
    IDX_TRY(idx_pubsub_open(&options, &pubsub, err));

    idx_status status = idx_pubsub_subscribe(pubsub, method,
                                             unsubscribe_method, params,
                                             handle, err);
    if (status != IDX_OK) {
        idx_pubsub_close(pubsub);
        return status;
    }

    *out = pubsub;
    return IDX_OK;
}

static void close_subscription(idx_pipeline *pipeline, idx_pubsub *pubsub,
                               uint64_t handle, idx_status status) {
    /* Tearing the subscription down is only worth attempting on a connection
     * that still works. */
    if (status == IDX_OK || stop_requested(pipeline)) {
        idx_pubsub_unsubscribe(pubsub, handle, NULL);
    }
    idx_pubsub_close(pubsub);
}

/* The hot path: whole blocks arrive on the socket (decision D1a). */
static idx_status run_subscription(idx_pipeline *pipeline, idx_error *err) {
    idx_pubsub *pubsub = NULL;
    uint64_t handle = 0;
    IDX_TRY(open_subscription(pipeline, "blockSubscribe", "blockUnsubscribe",
                              pipeline->subscribe_params, &pubsub, &handle,
                              err));
    IDX_INFO("following the tip over blockSubscribe (handle %llu)",
             (unsigned long long)handle);

    idx_status status = IDX_OK;
    while (!stop_requested(pipeline)) {
        idx_pubsub_message message;
        idx_status code = idx_pubsub_poll(
            pubsub, pipeline->options.poll_timeout_ms, &message, err);
        note_reconnects(pipeline, pubsub);

        if (code == IDX_ERR_TIMEOUT) {
            continue; /* nothing this round; the stop flag was just checked */
        }
        if (code != IDX_OK) {
            status = code;
            break;
        }

        pipeline->stats.bytes += message.raw.len;

        idx_slot slot = IDX_SLOT_NONE;
        idx_json_val block = {NULL};
        idx_error read_err;
        idx_error_clear(&read_err);
        idx_status read = idx_pipeline_read_block_notification(
            message.result, &slot, &block, &read_err);

        if (read != IDX_OK) {
            if (read == IDX_ERR_NOT_FOUND) {
                pipeline->stats.slots_skipped++;
                IDX_TRACE("%s", read_err.message);
            } else {
                IDX_WARN("ignoring a malformed notification: %s",
                         read_err.message);
            }
            idx_pubsub_message_free(&message);
            continue;
        }

        if (below_floor(pipeline, slot)) {
            IDX_TRACE("slot %llu is already indexed", (unsigned long long)slot);
            idx_pubsub_message_free(&message);
            continue;
        }
        if (above_end(pipeline, slot)) {
            idx_pubsub_message_free(&message);
            break;
        }

        /*
         * The document goes over to the other thread whole. It is a
         * self-contained parse, so nothing it points at belongs to the
         * connection, and handing it over saves both a copy and a second
         * parse (decision D6).
         */
        idx_ring_entry entry;
        memset(&entry, 0, sizeof(entry));
        entry.slot = slot;
        entry.doc = message.doc;
        entry.value = block;
        entry.tag = (uint64_t)IDX_BLOCK_FROM_SUBSCRIPTION;
        entry.bytes = message.raw.len;

        message.doc = NULL; /* ownership moves to the ring */
        idx_pubsub_message_free(&message);

        status = idx_ring_publish(pipeline->ring, &entry, err);
        if (status != IDX_OK) {
            break;
        }
        if (at_or_above_end(pipeline, slot)) {
            break;
        }
    }

    close_subscription(pipeline, pubsub, handle, status);
    return status;
}

/*
 * The fallback: slotSubscribe announces a slot and the block is fetched over
 * HTTP. This is what runs against an endpoint without blockSubscribe.
 */
static idx_status run_fallback(idx_pipeline *pipeline, idx_error *err) {
    const idx_config *cfg = pipeline->options.config;
    if (cfg->rpc_url[0] == '\0') {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "blockSubscribe is unavailable and no rpc endpoint is "
                        "configured to fall back to");
    }
    IDX_WARN("falling back to slotSubscribe and getBlock");
    pipeline->stats.used_fallback = true;

    const char *urls[1] = {cfg->rpc_url};
    idx_rpc_options rpc_options;
    idx_rpc_options_init(&rpc_options);
    rpc_options.urls = urls;
    rpc_options.url_count = 1;
    rpc_options.blocks_range_limit = cfg->blocks_range_limit;

    idx_rpc *rpc = NULL;
    IDX_TRY(idx_rpc_open(&rpc_options, &rpc, err));

    idx_pubsub *pubsub = NULL;
    uint64_t handle = 0;
    idx_status status = open_subscription(pipeline, "slotSubscribe",
                                          "slotUnsubscribe", "[]", &pubsub,
                                          &handle, err);
    if (status != IDX_OK) {
        idx_rpc_close(rpc);
        return status;
    }
    IDX_INFO("following the tip over slotSubscribe (handle %llu)",
             (unsigned long long)handle);

    idx_rpc_block_options block_options;
    idx_rpc_block_options_init(&block_options);
    block_options.commitment = cfg->commitment;
    block_options.transaction_details = idx_tx_details_name(cfg->tx_details);

    while (!stop_requested(pipeline)) {
        idx_pubsub_message message;
        idx_status code = idx_pubsub_poll(
            pubsub, pipeline->options.poll_timeout_ms, &message, err);
        note_reconnects(pipeline, pubsub);

        if (code == IDX_ERR_TIMEOUT) {
            continue;
        }
        if (code != IDX_OK) {
            status = code;
            break;
        }

        idx_slot slot = IDX_SLOT_NONE;
        idx_error read_err;
        idx_error_clear(&read_err);
        idx_status read =
            idx_pipeline_read_slot_notification(message.result, &slot,
                                                &read_err);
        idx_pubsub_message_free(&message);

        if (read != IDX_OK) {
            IDX_WARN("ignoring a malformed notification: %s",
                     read_err.message);
            continue;
        }

        if (below_floor(pipeline, slot)) {
            continue;
        }
        if (above_end(pipeline, slot)) {
            break;
        }

        idx_rpc_response response;
        idx_error fetch_err;
        idx_error_clear(&fetch_err);
        idx_status fetched =
            idx_rpc_get_block(rpc, slot, &block_options, &response, &fetch_err);

        if (fetched == IDX_ERR_NOT_FOUND) {
            pipeline->stats.slots_skipped++;
            IDX_TRACE("slot %llu was skipped", (unsigned long long)slot);
            continue;
        }
        if (fetched != IDX_OK) {
            /* One block the recovery path could not recover. It is a hole
             * like any other, and the gap item in M4 is what will fill it. */
            IDX_WARN("getBlock(%llu): %s", (unsigned long long)slot,
                     fetch_err.message);
            continue;
        }

        idx_ring_entry entry;
        memset(&entry, 0, sizeof(entry));
        entry.slot = slot;
        entry.doc = response.doc;
        entry.value = response.result;
        entry.tag = (uint64_t)IDX_BLOCK_FROM_RPC;
        entry.bytes = 0; /* the RPC client accounts for its own transfer */

        response.doc = NULL; /* ownership moves to the ring */
        idx_rpc_response_free(&response);

        status = idx_ring_publish(pipeline->ring, &entry, err);
        if (status != IDX_OK) {
            break;
        }
        if (at_or_above_end(pipeline, slot)) {
            break;
        }
    }

    close_subscription(pipeline, pubsub, handle, status);
    idx_rpc_close(rpc);
    return status;
}

/* -------------------------------------------------------------- public API -- */

void idx_pipeline_options_init(idx_pipeline_options *options) {
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->poll_timeout_ms = IDX_PIPELINE_POLL_TIMEOUT_MS;
    options->save_interval_ms = IDX_PIPELINE_SAVE_INTERVAL_MS;
    options->allow_fallback = true;
    options->recover_gaps = true;
}

idx_status idx_pipeline_open(const idx_pipeline_options *options,
                             idx_pipeline **out, idx_error *err) {
    if (options == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null argument");
    }
    if (options->config == NULL || options->cursor == NULL ||
        options->handler == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "pipeline needs a config, a cursor and a handler");
    }
    if (options->config->wss_url[0] == '\0') {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "no websocket endpoint configured");
    }

    idx_pipeline *pipeline = calloc(1, sizeof(*pipeline));
    if (pipeline == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "out of memory");
    }
    pipeline->options = *options;
    if (pipeline->options.poll_timeout_ms == 0) {
        pipeline->options.poll_timeout_ms = IDX_PIPELINE_POLL_TIMEOUT_MS;
    }
    if (pipeline->options.save_interval_ms == 0) {
        pipeline->options.save_interval_ms = IDX_PIPELINE_SAVE_INTERVAL_MS;
    }
    idx_error_clear(&pipeline->processor_err);

    /* Built here rather than per connection, so a subscription shape the
     * configuration cannot express fails before anything is opened. */
    idx_status status = idx_config_block_subscribe_params(
        options->config, pipeline->subscribe_params,
        sizeof(pipeline->subscribe_params), err);
    if (status != IDX_OK) {
        free(pipeline);
        return status;
    }

    idx_ring_options ring_options;
    idx_ring_options_init(&ring_options);
    if (options->config->queue_depth != 0) {
        ring_options.depth = options->config->queue_depth;
    }
    status = idx_ring_new(&ring_options, &pipeline->ring, err);
    if (status != IDX_OK) {
        free(pipeline);
        return status;
    }

    /*
     * The recovery ring blocks instead of dropping. Losing a recovered block
     * would put its slot straight back into the gap set to be fetched again,
     * and stalling a fetcher costs nothing — unlike stalling the socket
     * (decision D6). It is kept shallow because the fetchers are the ones
     * waiting on it.
     */
    idx_ring_options recovery_options;
    idx_ring_options_init(&recovery_options);
    recovery_options.depth = 4;
    recovery_options.block_when_full = true;
    status = idx_ring_new(&recovery_options, &pipeline->recovered, err);
    if (status != IDX_OK) {
        idx_ring_free(pipeline->ring);
        free(pipeline);
        return status;
    }

    status = idx_gaps_new(0, &pipeline->gaps, err);
    if (status != IDX_OK) {
        idx_ring_free(pipeline->recovered);
        idx_ring_free(pipeline->ring);
        free(pipeline);
        return status;
    }

    idx_arena_init(&pipeline->arena, IDX_ARENA_DEFAULT_CHUNK_SIZE);
    pipeline->stats.last_indexed = options->cursor->last_indexed;
    pipeline->highest_committed = options->cursor->last_indexed;

    *out = pipeline;
    return IDX_OK;
}

void idx_pipeline_close(idx_pipeline *pipeline) {
    if (pipeline == NULL) {
        return;
    }
    idx_fetcher_pool_stop(pipeline->fetchers);
    idx_ring_free(pipeline->recovered);
    idx_ring_free(pipeline->ring);
    idx_gaps_free(pipeline->gaps);
    idx_arena_destroy(&pipeline->arena);
    free(pipeline);
}

void idx_pipeline_request_stop(idx_pipeline *pipeline) {
    if (pipeline != NULL) {
        pipeline->stop = 1;
    }
}

void idx_pipeline_get_stats(const idx_pipeline *pipeline,
                            idx_pipeline_stats *out) {
    if (pipeline == NULL || out == NULL) {
        return;
    }
    *out = pipeline->stats;

    idx_ring_stats ring;
    memset(&ring, 0, sizeof(ring));
    idx_ring_get_stats(pipeline->ring, &ring);
    out->queue_dropped = ring.dropped;
    out->queue_high_water = ring.high_water;
    out->queue_depth = ring.depth;

    idx_gaps_stats gaps;
    memset(&gaps, 0, sizeof(gaps));
    idx_gaps_get_stats(pipeline->gaps, &gaps);
    out->gap_slots_outstanding = gaps.slot_count;
    out->gap_slots_resolved = gaps.resolved;
    out->gap_ranges = gaps.range_count;

    idx_fetcher_stats fetchers = pipeline->fetcher_stats;
    if (pipeline->fetchers != NULL) {
        idx_fetcher_pool_get_stats(pipeline->fetchers, &fetchers);
    }
    out->gap_slots_absent = fetchers.slots_absent;
    out->gap_fetch_failures = fetchers.fetch_failures;
    out->gap_ranges_abandoned = fetchers.ranges_abandoned;
    out->gap_ranges_claimed = fetchers.ranges_claimed;
}

/*
 * Everything between a resumed cursor and the current tip is a hole like any
 * other, so backfill is not a separate mode: the range goes into the gap set
 * and the same fetchers work it.
 */
static void queue_backfill(idx_pipeline *pipeline) {
    const idx_config *cfg = pipeline->options.config;

    if (!pipeline->options.recover_gaps || cfg->rpc_url[0] == '\0') {
        IDX_WARN("slots from %llu to the tip will not be backfilled: "
                 "gap recovery is off",
                 (unsigned long long)pipeline->floor);
        return;
    }

    const char *urls[1] = {cfg->rpc_url};
    idx_rpc_options rpc_options;
    idx_rpc_options_init(&rpc_options);
    rpc_options.urls = urls;
    rpc_options.url_count = 1;

    idx_rpc *rpc = NULL;
    idx_error err;
    idx_error_clear(&err);
    if (idx_rpc_open(&rpc_options, &rpc, &err) != IDX_OK) {
        IDX_WARN("cannot size the backfill: %s", err.message);
        return;
    }

    uint64_t tip = 0;
    idx_status status = idx_rpc_get_slot(rpc, cfg->commitment, &tip, &err);
    idx_rpc_close(rpc);

    if (status != IDX_OK) {
        IDX_WARN("cannot size the backfill: %s", err.message);
        return;
    }
    if (tip == 0 || tip <= pipeline->floor) {
        return; /* already at or ahead of the tip; nothing behind us */
    }

    /* The tip itself arrives on the socket, so the hole stops one short of
     * it, and never runs past a configured end slot. */
    idx_slot to = tip - 1;
    if (cfg->end_slot != 0 && to > cfg->end_slot) {
        to = cfg->end_slot;
    }
    if (to < pipeline->floor) {
        return;
    }

    if (idx_gaps_add(pipeline->gaps, pipeline->floor, to, &err) != IDX_OK) {
        IDX_ERROR("cannot queue the backfill: %s", err.message);
        return;
    }
    IDX_INFO("backfilling %llu..%llu (%llu slots) while following the tip",
             (unsigned long long)pipeline->floor, (unsigned long long)to,
             (unsigned long long)(to - pipeline->floor + 1));
}

static void start_fetchers(idx_pipeline *pipeline) {
    const idx_config *cfg = pipeline->options.config;

    if (!pipeline->options.recover_gaps) {
        IDX_WARN("gap recovery is off; holes will be recorded but not filled");
        return;
    }
    if (cfg->rpc_url[0] == '\0') {
        IDX_WARN("no rpc endpoint; holes will be recorded but not filled");
        return;
    }

    idx_fetcher_options options;
    idx_fetcher_options_init(&options);
    options.config = cfg;
    options.gaps = pipeline->gaps;
    options.output = pipeline->recovered;

    idx_error err;
    idx_error_clear(&err);
    if (idx_fetcher_pool_start(&options, &pipeline->fetchers, &err) != IDX_OK) {
        /* Degraded rather than fatal: the live path still works, and the gaps
         * stay recorded for a run that can fetch them. */
        IDX_WARN("cannot start the gap fetchers: %s", err.message);
        pipeline->fetchers = NULL;
    }
}

/*
 * Following the tip and recovering gaps finish independently: the live loop
 * returns the moment the tip reaches --end-slot, but the backfill it queued is
 * still being fetched. A bounded run's whole job is that backfill, so once
 * following is done cleanly, wait for the fetchers to drain the gap set before
 * the caller stops them — otherwise the range is abandoned half-fetched.
 *
 * The fetchers bound this themselves: a range that cannot be fetched is either
 * resolved as absent or abandoned after retries, so the set always empties. A
 * stop request (SIGINT, or a failed handler) still breaks out at once, leaving
 * the rest to the next run, which the cursor guarantees it will pick up.
 */
static void drain_recovery(idx_pipeline *pipeline) {
    if (pipeline->fetchers == NULL || idx_gaps_is_empty(pipeline->gaps)) {
        return;
    }

    idx_gaps_stats start;
    idx_gaps_get_stats(pipeline->gaps, &start);
    IDX_INFO("draining %llu backfill slots still outstanding",
             (unsigned long long)start.slot_count);

    double last_report = monotonic_seconds();
    while (!stop_requested(pipeline) && !idx_gaps_is_empty(pipeline->gaps)) {
        sleep_ms(100);

        double now = monotonic_seconds();
        if (now - last_report >= 5.0) {
            idx_gaps_stats current;
            idx_gaps_get_stats(pipeline->gaps, &current);
            IDX_INFO("draining: %llu backfill slots left",
                     (unsigned long long)current.slot_count);
            last_report = now;
        }
    }

    if (idx_gaps_is_empty(pipeline->gaps)) {
        IDX_INFO("backfill drained");
    }
}

idx_status idx_pipeline_run(idx_pipeline *pipeline, idx_error *err) {
    if (pipeline == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null pipeline");
    }

    pipeline->last_save = monotonic_seconds();
    const idx_slot_cursor *cursor = pipeline->options.cursor;
    pipeline->floor = idx_slot_cursor_resume_slot(cursor);

    if (pipeline->floor == 0) {
        IDX_INFO("following from the current tip");
    } else if (cursor->last_indexed != IDX_SLOT_NONE) {
        IDX_INFO("resuming at slot %llu (from cursor file %s)",
                 (unsigned long long)pipeline->floor, cursor->path);
        queue_backfill(pipeline);
    } else {
        IDX_INFO("resuming at slot %llu (--start-slot)",
                 (unsigned long long)pipeline->floor);
        queue_backfill(pipeline);
    }

    IDX_TRY(start_processor(pipeline, err));
    start_fetchers(pipeline);

    idx_status status = run_subscription(pipeline, err);

    if (status == IDX_ERR_REMOTE && pipeline->options.allow_fallback &&
        !stop_requested(pipeline)) {
        if (err != NULL && err->message[0] != '\0') {
            IDX_WARN("blockSubscribe was rejected: %s", err->message);
        }
        idx_error_clear(err);
        status = run_fallback(pipeline, err);
    }

    /*
     * Following ended. If it ended because the tip reached --end-slot (a clean
     * return, no stop pending), the backfill it queued may still be in flight;
     * let the fetchers finish it before they are stopped below.
     */
    if (status == IDX_OK && !stop_requested(pipeline)) {
        drain_recovery(pipeline);
    }

    /*
     * Shutdown order matters. The live ring closes first so nothing new is
     * fed. The fetchers stop next, while the processing thread is still
     * draining — a worker blocked on a full recovery ring needs a consumer to
     * make room for it, and stopping them the other way round would wedge.
     * Only then does the recovery ring close, which is what tells the
     * processing thread there is nothing left.
     */
    idx_ring_close(pipeline->ring);

    idx_fetcher_pool_get_stats(pipeline->fetchers, &pipeline->fetcher_stats);
    idx_fetcher_pool_stop(pipeline->fetchers);
    pipeline->fetchers = NULL;

    idx_ring_close(pipeline->recovered);
    pthread_join(pipeline->processor, NULL);
    pipeline->processor_running = false;

    /* A handler that failed is the more useful thing to report. */
    if (pipeline->processor_status != IDX_OK) {
        if (err != NULL) {
            *err = pipeline->processor_err;
        }
        return pipeline->processor_status;
    }
    return status;
}
