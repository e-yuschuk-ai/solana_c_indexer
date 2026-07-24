#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "balance.h"
#include "block.h"
#include "config.h"
#include "error.h"
#include "log.h"
#include "pipeline.h"
#include "slot_cursor.h"
#include "transfer.h"
#include "venue.h"
#include "version.h"
#include "vote_filter.h"

/*
 * The signal handler needs a way to reach the pipeline, and a signal handler
 * takes no arguments. This is the only global in the program, and the only
 * thing the handler touches.
 */
static idx_pipeline *volatile g_pipeline = NULL;

static void on_signal(int signal_number) {
    (void)signal_number;
    idx_pipeline_request_stop(g_pipeline);
}

/* How often the progress line is written, and how long the reporter sleeps
 * between checks of the stop flag — short, so shutdown is not held up by it. */
#define IDX_PROGRESS_INTERVAL_SECONDS 5.0
#define IDX_PROGRESS_TICK_MS 200

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* What the consumer stub accumulates across the run. */
typedef struct {
    uint64_t transactions;    /* what survived the vote filter */
    uint64_t votes;           /* what it dropped */
    uint64_t sol_balances;    /* balance rows the survivors produced */
    uint64_t token_balances;  /* the same, per token account */
    uint64_t sol_transfers;   /* lamport movements from System instructions */
    uint64_t token_transfers; /* token movements, mints and burns included */
    uint64_t swaps[IDX_VENUE_JUPITER + 1]; /* one counter per venue */
    uint64_t swap_failures; /* payloads a venue decoder recognised but could
                             * not read: a layout that has drifted */
} idx_tally;

/* Counts the swaps of one transaction, by venue. Every instruction is offered
 * to the venue decoders, top level and inner alike: a route's legs and a
 * wallet's direct trades arrive at different depths. */
static void count_swaps(const idx_transaction *tx, idx_tally *tally) {
    for (size_t i = 0; i < tx->instruction_count + tx->inner_instruction_count;
         i++) {
        const idx_instruction *list = NULL;
        size_t count = 0;
        if (i < tx->instruction_count) {
            list = &tx->instructions[i];
            count = 1;
        } else {
            const idx_inner_instructions *group =
                &tx->inner_instructions[i - tx->instruction_count];
            list = group->instructions;
            count = group->instruction_count;
        }
        for (size_t j = 0; j < count; j++) {
            idx_swap swap;
            /* Anything that is not a swap comes back not-found, which is the
             * answer for almost every instruction in a block. Anything else is
             * a payload a decoder claimed and then could not read, which is
             * what a program upgrade looks like from here. */
            idx_status swap_status = idx_swap_decode(tx, &list[j], &swap, NULL);
            if (swap_status == IDX_OK) {
                tally->swaps[swap.venue]++;
            } else if (swap_status != IDX_ERR_NOT_FOUND) {
                tally->swap_failures++;
            }
        }
    }
}

/*
 * How far behind the chain a block is: wall clock against the timestamp the
 * chain itself put on it. Written as text because the answer is sometimes that
 * there is no answer — blockTime is optional, and a block without one leaves
 * the lag unknown rather than zero.
 */
static void format_lag(char *out, size_t size, int64_t block_time,
                       bool has_block_time) {
    if (!has_block_time) {
        snprintf(out, size, "unknown");
        return;
    }
    snprintf(out, size, "%lld s", (long long)time(NULL) - (long long)block_time);
}

/*
 * What the reporter thread needs to see. The counters it reads are written by
 * the processing thread without synchronization, exactly as documented for
 * idx_pipeline_get_stats: a reading may be a few blocks stale, and nothing
 * decides anything on it.
 */
typedef struct {
    const idx_pipeline *pipeline;
    const idx_tally *tally;
    volatile sig_atomic_t stop;
} idx_progress;

static void sleep_ms(int milliseconds) {
    struct timespec request;
    request.tv_sec = milliseconds / 1000;
    request.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&request, NULL);
}

/*
 * Where the indexer is, in the terms an operator actually asks about: how far
 * behind the chain it is right now, and how fast it is moving. The lag is wall
 * clock against the chain's own timestamp for the newest block committed, so it
 * keeps climbing while the stream is stalled instead of standing still at
 * whatever the last block said.
 */
static void report_progress(double window_seconds, uint64_t blocks,
                            uint64_t transactions,
                            const idx_pipeline_stats *stats) {
    char lag[32];
    format_lag(lag, sizeof(lag), stats->tip_block_time,
               stats->has_tip_block_time);

    /* Slots between the tip and the durable frontier: what a backfill still
     * owes, and 0 once it has caught up. */
    unsigned long long behind = 0;
    if (stats->last_indexed != IDX_SLOT_NONE &&
        stats->tip_slot > stats->last_indexed) {
        behind = (unsigned long long)(stats->tip_slot - stats->last_indexed);
    }

    /* The transaction rate is what survived the vote filter, the same number
     * the summary reports at exit — counting votes here would say more about
     * the validators than about the indexer. */
    IDX_INFO("progress: slot %llu, lag %s, %.1f blocks/s, %.0f txn/s, "
             "%llu slots behind",
             (unsigned long long)stats->tip_slot, lag,
             (window_seconds > 0.0) ? (double)blocks / window_seconds : 0.0,
             (window_seconds > 0.0) ? (double)transactions / window_seconds : 0.0,
             behind);
}

static void *run_reporter(void *argument) {
    idx_progress *progress = (idx_progress *)argument;

    double last = monotonic_seconds();
    uint64_t last_blocks = 0;
    uint64_t last_transactions = 0;

    while (!progress->stop) {
        sleep_ms(IDX_PROGRESS_TICK_MS);

        double now = monotonic_seconds();
        double window = now - last;
        if (window < IDX_PROGRESS_INTERVAL_SECONDS) {
            continue;
        }

        idx_pipeline_stats stats;
        idx_pipeline_get_stats(progress->pipeline, &stats);
        uint64_t transactions = progress->tally->transactions;

        /* Nothing has been committed yet: the subscription is still coming up,
         * and the transport says so at its own level. */
        if (stats.tip_slot != IDX_SLOT_NONE) {
            report_progress(window, stats.blocks - last_blocks,
                            transactions - last_transactions, &stats);
        }

        last = now;
        last_blocks = stats.blocks;
        last_transactions = transactions;
    }
    return NULL;
}

/*
 * Consumer stub. Storing decoded transactions is M7; until then the handler
 * decodes the block (M5), drops votes and extracts balances (M6), and tallies
 * what came out, which
 * both proves the decoder against live data and is what advances the cursor. A
 * decode failure stops the pipeline, leaving the cursor on the offending slot
 * — the strict choice that surfaces bugs while the decoder is being built out.
 */
static idx_status count_block(const idx_raw_block *block, void *user,
                              idx_error *err) {
    idx_tally *tally = (idx_tally *)user;

    idx_block decoded;
    idx_status status =
        idx_block_decode(block->value, block->slot, block->arena, &decoded, err);
    if (status != IDX_OK) {
        IDX_WARN("slot %llu: decode failed: %s",
                 (unsigned long long)block->slot,
                 (err != NULL) ? err->message : "");
        return status;
    }

    size_t instructions = 0;
    size_t inner = 0;
    size_t versioned = 0;
    uint64_t fees = 0;
    size_t token_balances = 0;
    size_t logs = 0;
    size_t votes = 0;
    size_t sol_balances = 0;
    size_t sol_transfers = 0;
    size_t token_transfers = 0;
    for (size_t i = 0; i < decoded.transaction_count; i++) {
        const idx_transaction *tx = &decoded.transactions[i];
        if (idx_vote_filter_should_drop(tx)) {
            votes++;
            continue;
        }

        /* Allocated from the handler's arena, which is reset the moment this
         * returns — which is all the lifetime a count needs. */
        const idx_sol_balance *balances = NULL;
        size_t balance_count = 0;
        status = idx_sol_balance_extract(tx, block->arena, &balances,
                                         &balance_count, err);
        if (status != IDX_OK) {
            IDX_WARN("slot %llu: balance extraction failed: %s",
                     (unsigned long long)block->slot,
                     (err != NULL) ? err->message : "");
            return status;
        }
        sol_balances += balance_count;

        const idx_token_balance_state *token_states = NULL;
        size_t token_state_count = 0;
        status = idx_token_balance_extract(tx, block->arena, &token_states,
                                           &token_state_count, err);
        if (status != IDX_OK) {
            IDX_WARN("slot %llu: token balance extraction failed: %s",
                     (unsigned long long)block->slot,
                     (err != NULL) ? err->message : "");
            return status;
        }
        token_balances += token_state_count;

        const idx_transfer *moves = NULL;
        size_t move_count = 0;
        status = idx_transfer_extract(tx, block->arena, &moves, &move_count,
                                      err);
        if (status != IDX_OK) {
            IDX_WARN("slot %llu: transfer extraction failed: %s",
                     (unsigned long long)block->slot,
                     (err != NULL) ? err->message : "");
            return status;
        }
        /* The two the storage tiers keep apart (D5), counted apart here so
         * their volumes are visible against live data before M7 sizes for
         * them. */
        for (size_t j = 0; j < move_count; j++) {
            if (moves[j].kind == IDX_TRANSFER_SOL) {
                sol_transfers++;
            } else {
                token_transfers++;
            }
        }

        count_swaps(tx, tally);

        instructions += tx->instruction_count;
        for (size_t j = 0; j < tx->inner_instruction_count; j++) {
            inner += tx->inner_instructions[j].instruction_count;
        }
        if (tx->version != IDX_TX_VERSION_LEGACY) {
            versioned++;
        }
        fees += tx->fee;
        logs += tx->log_count;
    }

    tally->transactions += decoded.transaction_count - votes;
    tally->votes += votes;
    tally->sol_balances += sol_balances;
    tally->token_balances += token_balances;
    tally->sol_transfers += sol_transfers;
    tally->token_transfers += token_transfers;

    char lag[32];
    format_lag(lag, sizeof(lag), decoded.block_time, decoded.has_block_time);
    IDX_DEBUG("slot %llu: %zu txns (%zu votes dropped, %zu v0), %zu ix, "
              "%zu inner, %llu lamports fees, %zu sol balances, "
              "%zu token balances, %zu sol transfers, %zu token transfers, "
              "%zu logs, %.2f MiB from %s, lag %s",
              (unsigned long long)block->slot, decoded.transaction_count,
              votes, versioned, instructions, inner, (unsigned long long)fees,
              sol_balances, token_balances, sol_transfers, token_transfers,
              logs,
              (double)block->bytes / (1024.0 * 1024.0),
              idx_block_origin_name(block->origin), lag);
    return IDX_OK;
}

int main(int argc, char **argv) {
    idx_error err;
    idx_error_clear(&err);

    /* Log at INFO until the configured level is known. */
    idx_log_init(stderr, IDX_LOG_INFO);

    idx_config cfg;
    if (idx_config_load(&cfg, argc, argv, &err) != IDX_OK) {
        fprintf(stderr, "configuration error: %s\n", err.message);
        return EXIT_FAILURE;
    }

    if (cfg.help) {
        idx_config_usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }

    idx_log_set_level(cfg.log_level);

    if (idx_config_validate(&cfg, &err) != IDX_OK) {
        IDX_ERROR("configuration error: %s", err.message);
        return EXIT_FAILURE;
    }

    IDX_INFO("solana_c_indexer %s starting", IDX_VERSION_STRING);
    idx_config_log(&cfg);

    idx_slot_cursor cursor;
    if (idx_slot_cursor_open(&cursor, cfg.state_file, cfg.start_slot, &err) !=
        IDX_OK) {
        IDX_ERROR("cannot open the slot cursor: %s", err.message);
        return EXIT_FAILURE;
    }

    idx_tally tally;
    memset(&tally, 0, sizeof(tally));

    idx_pipeline_options options;
    idx_pipeline_options_init(&options);
    options.config = &cfg;
    options.cursor = &cursor;
    options.handler = count_block;
    options.user = &tally;

    idx_pipeline *pipeline = NULL;
    if (idx_pipeline_open(&options, &pipeline, &err) != IDX_OK) {
        IDX_ERROR("cannot start the pipeline: %s", err.message);
        return EXIT_FAILURE;
    }

    /* Installed only once the pipeline exists, so the handler never sees a
     * half-built one. */
    g_pipeline = pipeline;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /*
     * The run loop owns this thread from here on, so the progress line comes
     * from a second one. It only reads counters, and a failure to start it
     * costs visibility rather than indexing.
     */
    idx_progress progress;
    progress.pipeline = pipeline;
    progress.tally = &tally;
    progress.stop = 0;

    pthread_t reporter;
    bool reporting = pthread_create(&reporter, NULL, run_reporter, &progress) == 0;
    if (!reporting) {
        IDX_WARN("cannot start the progress reporter; continuing without it");
    }

    double started = monotonic_seconds();
    idx_status status = idx_pipeline_run(pipeline, &err);
    double elapsed = monotonic_seconds() - started;

    if (reporting) {
        progress.stop = 1;
        pthread_join(reporter, NULL);
    }

    idx_pipeline_stats stats;
    idx_pipeline_get_stats(pipeline, &stats);

    IDX_INFO("indexed %llu blocks (%llu transactions, %llu votes dropped, "
             "%llu sol balances, %llu token balances, %llu sol transfers, "
             "%llu token transfers) in %.1f s, %.2f blocks/s",
             (unsigned long long)stats.blocks,
             (unsigned long long)tally.transactions,
             (unsigned long long)tally.votes,
             (unsigned long long)tally.sol_balances,
             (unsigned long long)tally.token_balances,
             (unsigned long long)tally.sol_transfers,
             (unsigned long long)tally.token_transfers, elapsed,
             (elapsed > 0.0) ? (double)stats.blocks / elapsed : 0.0);
    for (idx_venue venue = IDX_VENUE_PUMP_CURVE; venue <= IDX_VENUE_JUPITER;
         venue++) {
        if (tally.swaps[venue] != 0) {
            IDX_INFO("swaps: %-14s %llu", idx_venue_name(venue),
                     (unsigned long long)tally.swaps[venue]);
        }
    }
    if (tally.swap_failures != 0) {
        IDX_WARN("swaps: %llu payloads a venue decoder could not read",
                 (unsigned long long)tally.swap_failures);
    }
    IDX_INFO("skipped=%llu missed=%llu reconnects=%llu socket=%.1f MiB",
             (unsigned long long)stats.slots_skipped,
             (unsigned long long)stats.slots_missed,
             (unsigned long long)stats.reconnects,
             (double)stats.bytes / (1024.0 * 1024.0));
    IDX_INFO("queue: dropped=%llu high_water=%zu of %zu",
             (unsigned long long)stats.queue_dropped, stats.queue_high_water,
             stats.queue_depth);
    IDX_INFO("gaps: claimed=%llu recovered=%llu absent=%llu resolved=%llu "
             "failures=%llu abandoned=%llu",
             (unsigned long long)stats.gap_ranges_claimed,
             (unsigned long long)stats.blocks_recovered,
             (unsigned long long)stats.gap_slots_absent,
             (unsigned long long)stats.gap_slots_resolved,
             (unsigned long long)stats.gap_fetch_failures,
             (unsigned long long)stats.gap_ranges_abandoned);
    if (stats.gap_slots_outstanding != 0) {
        /* Still missing at exit, so the next run has to pick them up — which
         * it will, because the cursor never advanced past them. */
        IDX_WARN("gaps: %llu slots still outstanding in %zu ranges",
                 (unsigned long long)stats.gap_slots_outstanding,
                 stats.gap_ranges);
    }
    if (stats.last_indexed != IDX_SLOT_NONE) {
        IDX_INFO("last indexed slot %llu",
                 (unsigned long long)stats.last_indexed);
    }
    if (stats.tip_slot != IDX_SLOT_NONE) {
        char lag[32];
        format_lag(lag, sizeof(lag), stats.tip_block_time,
                   stats.has_tip_block_time);
        IDX_INFO("newest block seen: slot %llu, lag %s at exit",
                 (unsigned long long)stats.tip_slot, lag);
    }

    g_pipeline = NULL;
    idx_pipeline_close(pipeline);

    if (status != IDX_OK) {
        IDX_ERROR("%s", err.message);
        return EXIT_FAILURE;
    }
    IDX_INFO("shutting down");
    return EXIT_SUCCESS;
}
