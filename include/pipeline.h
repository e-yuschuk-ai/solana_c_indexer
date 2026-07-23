/*
 * Ingestion pipeline: follow mode (ROADMAP.md milestone M4).
 *
 * Connects the transport layer to a consumer. `blockSubscribe` delivers blocks
 * on the socket (decision D1a); when the endpoint does not offer it, the
 * pipeline falls back to `slotSubscribe` plus `getBlock` over HTTP. Either way
 * the consumer sees the same thing: one call per block, in slot order.
 *
 * Two threads (decision D6). idx_pipeline_run keeps the receive loop on the
 * calling thread and starts a second one to process; a bounded ring sits
 * between them, and a receive loop that would have to wait drops its oldest
 * queued block instead. The socket is never made to wait for the handler.
 *
 * The handler therefore runs on the processing thread, not the caller's. It is
 * the only thing that touches the slot cursor, which is what keeps the two
 * threads from sharing any mutable state at all.
 *
 * The pipeline owns the position on the chain. It observes every slot that
 * reaches the processing thread, and advances the indexed frontier only once
 * the handler has accepted the block, so a restart resumes at the first slot
 * that was never committed rather than at the last one that arrived.
 *
 * What it does not do yet: fetching the gaps, whether the socket missed them or
 * the ring dropped them. That is an M4 item still open; for now they are
 * counted and logged.
 *
 * One owner per instance. idx_pipeline_request_stop may additionally be called
 * from a signal handler, and idx_pipeline_get_stats from a monitoring thread.
 */
#ifndef IDX_PIPELINE_H
#define IDX_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "bytes.h"
#include "config.h"
#include "error.h"
#include "json.h"
#include "slot_cursor.h"
#include "types.h"

typedef struct idx_pipeline idx_pipeline;

/* Which transport delivered a block. */
typedef enum {
    IDX_BLOCK_FROM_SUBSCRIPTION = 0, /* blockSubscribe, the hot path */
    IDX_BLOCK_FROM_RPC               /* getBlock, the recovery path */
} idx_block_origin;

/* Lowercase name, for log messages. Never returns NULL. */
const char *idx_block_origin_name(idx_block_origin origin);

/*
 * One block, still in its JSON form: decoding is M5. Everything here borrows
 * storage owned by the pipeline and is only valid for the duration of the
 * handler call.
 */
typedef struct {
    idx_slot slot;

    /* The block object: blockhash, parentSlot, transactions, blockTime. */
    idx_json_val value;

    /* Size of the payload this was parsed from. The bytes themselves are gone
     * — only the parsed document crosses between the threads — and this is 0
     * on the RPC path, where the transfer is the RPC client's to account for. */
    size_t bytes;

    idx_block_origin origin;

    /* Scratch memory for the handler, reset as soon as it returns. */
    idx_arena *arena;
} idx_raw_block;

/*
 * Called once per block, on the processing thread. Returning IDX_OK means the
 * block is committed, and is what advances the indexed frontier; returning
 * anything else stops the pipeline and is reported from idx_pipeline_run,
 * leaving the cursor pointing at the block that failed so a restart retries it.
 */
typedef idx_status (*idx_block_handler)(const idx_raw_block *block, void *user,
                                        idx_error *err);

typedef struct {
    /* Required, and borrowed: both must outlive the pipeline. */
    const idx_config *config;
    idx_slot_cursor *cursor;

    idx_block_handler handler; /* required */
    void *user;                /* passed through untouched */

    /* How long a poll blocks before the loop checks for a stop request.
     * Detecting a dead connection belongs to the pubsub layer, so this only
     * bounds how promptly a shutdown is noticed. 0 selects 1 s. */
    int poll_timeout_ms;

    /* Upper bound on how often the cursor is persisted. 0 selects 1 s; a
     * negative value saves after every block. */
    int save_interval_ms;

    /* Try slotSubscribe + getBlock when blockSubscribe is unavailable. Needs
     * an RPC endpoint to be configured. */
    bool allow_fallback;
} idx_pipeline_options;

/* Defaults: 1 s poll, 1 s save interval, fallback enabled. */
void idx_pipeline_options_init(idx_pipeline_options *options);

typedef struct {
    uint64_t blocks;         /* committed by the handler */
    uint64_t slots_skipped;  /* notified, but the chain produced no block */
    uint64_t slots_missed;   /* holes between consecutive notifications */
    uint64_t handler_errors;
    uint64_t save_failures;
    /* Block payload delivered on the socket. Stays 0 on the fallback path,
     * where the socket only carries slot numbers; those bytes are the RPC
     * client's to account for. */
    uint64_t bytes;
    uint64_t reconnects;

    /* Blocks the ring dropped because the processing thread was behind. Each
     * one is a slot to refetch, not lost data (decision D6). */
    uint64_t queue_dropped;
    size_t queue_high_water; /* deepest the ring has been */
    size_t queue_depth;      /* what the ring was sized for */

    idx_slot last_indexed;   /* IDX_SLOT_NONE until the first commit */
    bool used_fallback;
} idx_pipeline_stats;

/*
 * Validates the options and allocates. Nothing is connected here, so the
 * failures this reports are configuration problems: a missing endpoint, a
 * subscription shape the config cannot express.
 */
idx_status idx_pipeline_open(const idx_pipeline_options *options,
                             idx_pipeline **out, idx_error *err);

void idx_pipeline_close(idx_pipeline *pipeline);

/*
 * Starts the processing thread and follows the tip on the calling thread until
 * a stop is requested, the configured end slot is reached, or something fails.
 * Returns only once the processing thread has drained what was queued.
 *
 *   IDX_OK          stopped cleanly
 *   IDX_ERR_REMOTE  the endpoint rejected the subscription and no fallback was
 *                   available
 *   anything else   the failure that ended the run; the handler's own status
 *                   is propagated unchanged
 *
 * The cursor is persisted before returning, whatever the outcome.
 */
idx_status idx_pipeline_run(idx_pipeline *pipeline, idx_error *err);

/*
 * Asks the run loop to stop at the next block boundary. Async-signal-safe, and
 * the only function that may be called from another context than the owner.
 */
void idx_pipeline_request_stop(idx_pipeline *pipeline);

/*
 * A monitoring snapshot. Each counter has a single writer, but the two threads
 * write different ones, so a reader may catch a slightly mixed picture. That is
 * what these are for; nothing decides anything on them.
 */
void idx_pipeline_get_stats(const idx_pipeline *pipeline,
                            idx_pipeline_stats *out);

/* ------------------------------------------------------- notification reads -- */

/*
 * Reads `params.result` of a `blockNotification`. Exposed so the shape can be
 * tested without a live endpoint.
 *
 *   IDX_OK            `slot` and `block` are set
 *   IDX_ERR_NOT_FOUND the slot carries no block: it was skipped, or the
 *                     notification reports an error for it. `slot` is still
 *                     set, because the slot itself was observed
 *   IDX_ERR_PARSE     the notification does not have the documented shape
 */
idx_status idx_pipeline_read_block_notification(idx_json_val result,
                                                idx_slot *slot,
                                                idx_json_val *block,
                                                idx_error *err);

/* Reads `params.result` of a `slotNotification`. IDX_ERR_PARSE if malformed. */
idx_status idx_pipeline_read_slot_notification(idx_json_val result,
                                               idx_slot *slot, idx_error *err);

#endif /* IDX_PIPELINE_H */
