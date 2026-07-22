#include <stdio.h>
#include <stdlib.h>

#include "arena.h"
#include "config.h"
#include "error.h"
#include "log.h"
#include "version.h"

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

    /*
     * The per-block arena is created here so its lifetime matches the process.
     * M4 hands it to the ingestion pipeline; for now it only proves the
     * allocator is wired up.
     */
    idx_arena arena;
    idx_arena_init(&arena, IDX_ARENA_DEFAULT_CHUNK_SIZE);
    IDX_DEBUG("arena ready, chunk size %zu bytes", arena.chunk_size);
    idx_arena_destroy(&arena);

    IDX_WARN("no ingestion pipeline yet; see ROADMAP.md milestone M4");
    IDX_INFO("shutting down");
    return EXIT_SUCCESS;
}
