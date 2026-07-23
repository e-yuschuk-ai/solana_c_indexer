#include "pipeline.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "pubsub.h"
#include "rpc.h"

/*
 * How long the loop blocks before looking at the stop flag. Noticing a dead
 * connection is the pubsub layer's job, not this one's, so the only thing this
 * bounds is how long a shutdown waits — and one idle poll per second costs
 * nothing next to 12 MiB/s of blocks.
 */
#define IDX_PIPELINE_POLL_TIMEOUT_MS 1000
#define IDX_PIPELINE_SAVE_INTERVAL_MS 1000

/* Room for the filter — a base58 pubkey at most — plus the options object. */
#define IDX_PIPELINE_PARAMS_MAX (IDX_CONFIG_STR_MAX + 128)

struct idx_pipeline {
    idx_pipeline_options options;
    idx_arena arena;
    idx_pipeline_stats stats;

    char subscribe_params[IDX_PIPELINE_PARAMS_MAX];

    /* Lowest slot worth indexing. 0 means "wherever the tip is". */
    idx_slot floor;

    /* Written by idx_pipeline_request_stop, which may be a signal handler. */
    volatile sig_atomic_t stop;

    double last_save; /* monotonic seconds */
    bool save_warned;
};

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
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

/* -------------------------------------------------------------- bookkeeping -- */

static bool stop_requested(const idx_pipeline *pipeline) {
    return pipeline->stop != 0;
}

/* True once the configured end slot has been reached, by either frontier: a
 * skipped end slot must still end the run. */
static bool reached_end(const idx_pipeline *pipeline) {
    idx_slot end = pipeline->options.config->end_slot;
    if (end == 0) {
        return false;
    }
    const idx_slot_cursor *cursor = pipeline->options.cursor;
    return (cursor->last_indexed != IDX_SLOT_NONE &&
            cursor->last_indexed >= end) ||
           (cursor->last_seen != IDX_SLOT_NONE && cursor->last_seen >= end);
}

static bool above_end(const idx_pipeline *pipeline, idx_slot slot) {
    idx_slot end = pipeline->options.config->end_slot;
    return end != 0 && slot > end;
}

static bool below_floor(const idx_pipeline *pipeline, idx_slot slot) {
    return pipeline->floor != 0 && slot < pipeline->floor;
}

static void observe_slot(idx_pipeline *pipeline, idx_slot slot) {
    idx_slot_cursor *cursor = pipeline->options.cursor;
    idx_slot previous = cursor->last_seen;

    if (previous != IDX_SLOT_NONE && slot > previous + 1) {
        /* Counted so the hole is at least visible. Fetching it is the gap
         * item in M4 and is not implemented yet. */
        pipeline->stats.slots_missed += slot - previous - 1;
        IDX_DEBUG("gap: slots %llu..%llu were not delivered",
                  (unsigned long long)(previous + 1),
                  (unsigned long long)(slot - 1));
    }
    idx_slot_cursor_observe(cursor, slot);
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
    idx_slot_cursor_record_indexed(pipeline->options.cursor, block->slot);
    pipeline->stats.last_indexed = pipeline->options.cursor->last_indexed;
    save_cursor(pipeline, false);
    return IDX_OK;
}

static void note_reconnects(idx_pipeline *pipeline, const idx_pubsub *pubsub) {
    idx_pubsub_stats stats;
    idx_pubsub_get_stats(pubsub, &stats);
    if (stats.reconnects <= pipeline->stats.reconnects) {
        return;
    }
    pipeline->stats.reconnects = stats.reconnects;

    idx_slot seen = pipeline->options.cursor->last_seen;
    if (seen == IDX_SLOT_NONE) {
        IDX_WARN("reconnected before any slot arrived");
    } else {
        IDX_WARN("reconnected; slots after %llu were not delivered",
                 (unsigned long long)seen);
    }
}

/* ---------------------------------------------------------------- run loops -- */

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
    while (!stop_requested(pipeline) && !reached_end(pipeline)) {
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

        if (slot != IDX_SLOT_NONE) {
            observe_slot(pipeline, slot);
        }

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

        idx_raw_block raw;
        raw.slot = slot;
        raw.value = block;
        raw.raw = message.raw;
        raw.origin = IDX_BLOCK_FROM_SUBSCRIPTION;
        raw.arena = &pipeline->arena;

        status = commit_block(pipeline, &raw, err);
        idx_pubsub_message_free(&message);
        if (status != IDX_OK) {
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

    while (!stop_requested(pipeline) && !reached_end(pipeline)) {
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

        observe_slot(pipeline, slot);
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
            pipeline->stats.slots_missed++;
            IDX_WARN("getBlock(%llu): %s", (unsigned long long)slot,
                     fetch_err.message);
            continue;
        }

        idx_raw_block raw;
        raw.slot = slot;
        raw.value = response.result;
        raw.raw = idx_slice_make(NULL, 0);
        raw.origin = IDX_BLOCK_FROM_RPC;
        raw.arena = &pipeline->arena;

        status = commit_block(pipeline, &raw, err);
        idx_rpc_response_free(&response);
        if (status != IDX_OK) {
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

    /* Built here rather than per connection, so a subscription shape the
     * configuration cannot express fails before anything is opened. */
    idx_status status = idx_config_block_subscribe_params(
        options->config, pipeline->subscribe_params,
        sizeof(pipeline->subscribe_params), err);
    if (status != IDX_OK) {
        free(pipeline);
        return status;
    }

    idx_arena_init(&pipeline->arena, IDX_ARENA_DEFAULT_CHUNK_SIZE);
    pipeline->stats.last_indexed = options->cursor->last_indexed;

    *out = pipeline;
    return IDX_OK;
}

void idx_pipeline_close(idx_pipeline *pipeline) {
    if (pipeline == NULL) {
        return;
    }
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
}

idx_status idx_pipeline_run(idx_pipeline *pipeline, idx_error *err) {
    if (pipeline == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null pipeline");
    }

    pipeline->last_save = monotonic_seconds();
    pipeline->floor = idx_slot_cursor_resume_slot(pipeline->options.cursor);

    if (pipeline->floor == 0) {
        IDX_INFO("following from the current tip");
    } else {
        IDX_INFO("resuming at slot %llu", (unsigned long long)pipeline->floor);
        /*
         * Whatever sits between the floor and the tip is a hole that follow
         * mode alone does not close, because the socket only carries what
         * happens next. The backfill item in M4 is what closes it.
         */
        IDX_WARN("slots between %llu and the tip are not backfilled yet "
                 "(ROADMAP.md milestone M4)",
                 (unsigned long long)pipeline->floor);
    }

    idx_status status = run_subscription(pipeline, err);

    if (status == IDX_ERR_REMOTE && pipeline->options.allow_fallback &&
        !stop_requested(pipeline)) {
        if (err != NULL && err->message[0] != '\0') {
            IDX_WARN("blockSubscribe was rejected: %s", err->message);
        }
        idx_error_clear(err);
        status = run_fallback(pipeline, err);
    }

    save_cursor(pipeline, true);
    return status;
}
