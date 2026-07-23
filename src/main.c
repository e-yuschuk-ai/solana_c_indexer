#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "error.h"
#include "log.h"
#include "pipeline.h"
#include "slot_cursor.h"
#include "version.h"

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

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/*
 * Consumer stub. Decoding a block into transactions is M5 and storing them is
 * M7; until then the handler proves the block arrived intact and readable, and
 * accepting it is what advances the cursor.
 */
static idx_status count_block(const idx_raw_block *block, void *user,
                              idx_error *err) {
    (void)err;
    uint64_t *transactions = (uint64_t *)user;

    size_t count = idx_json_array_size(idx_json_get(block->value,
                                                    "transactions"));
    *transactions += count;

    IDX_DEBUG("slot %llu: %zu transactions, %.2f MiB from %s",
              (unsigned long long)block->slot, count,
              (double)block->bytes / (1024.0 * 1024.0),
              idx_block_origin_name(block->origin));
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

    uint64_t transactions = 0;

    idx_pipeline_options options;
    idx_pipeline_options_init(&options);
    options.config = &cfg;
    options.cursor = &cursor;
    options.handler = count_block;
    options.user = &transactions;

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

    double started = monotonic_seconds();
    idx_status status = idx_pipeline_run(pipeline, &err);
    double elapsed = monotonic_seconds() - started;

    idx_pipeline_stats stats;
    idx_pipeline_get_stats(pipeline, &stats);

    IDX_INFO("indexed %llu blocks (%llu transactions) in %.1f s, %.2f blocks/s",
             (unsigned long long)stats.blocks,
             (unsigned long long)transactions, elapsed,
             (elapsed > 0.0) ? (double)stats.blocks / elapsed : 0.0);
    IDX_INFO("skipped=%llu missed=%llu reconnects=%llu socket=%.1f MiB",
             (unsigned long long)stats.slots_skipped,
             (unsigned long long)stats.slots_missed,
             (unsigned long long)stats.reconnects,
             (double)stats.bytes / (1024.0 * 1024.0));
    IDX_INFO("queue: dropped=%llu high_water=%zu of %zu",
             (unsigned long long)stats.queue_dropped, stats.queue_high_water,
             stats.queue_depth);
    if (stats.last_indexed != IDX_SLOT_NONE) {
        IDX_INFO("last indexed slot %llu",
                 (unsigned long long)stats.last_indexed);
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
