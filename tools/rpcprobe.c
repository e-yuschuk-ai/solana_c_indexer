/*
 * Exercises the HTTP JSON-RPC client against a live endpoint.
 *
 *   ./build/debug/rpcprobe
 *
 * Runs the methods the ingestion pipeline depends on, including the cases that
 * matter but are awkward to reach on purpose: a skipped slot, and a batch.
 * The endpoint comes from SOLANA_RPC_URL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "rpc.h"

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
    idx_log_init(stderr, IDX_LOG_INFO);

    const char *url = getenv("SOLANA_RPC_URL");
    if (url == NULL || *url == '\0') {
        fprintf(stderr, "SOLANA_RPC_URL is not set\n");
        return EXIT_FAILURE;
    }

    const char *urls[] = {url};
    idx_rpc_options options;
    idx_rpc_options_init(&options);
    options.urls = urls;
    options.url_count = 1;
    /* Free plans cap getBlocks hard; the client splits wider ranges itself. */
    const char *limit = getenv("INDEXER_BLOCKS_RANGE_LIMIT");
    if (limit != NULL) {
        options.blocks_range_limit = strtoull(limit, NULL, 10);
    }

    idx_error err;
    idx_error_clear(&err);

    idx_rpc *rpc = NULL;
    if (idx_rpc_open(&options, &rpc, &err) != IDX_OK) {
        IDX_ERROR("%s", err.message);
        return EXIT_FAILURE;
    }

    int failures = 0;

    /* --- health and version --- */
    if (idx_rpc_get_health(rpc, &err) == IDX_OK) {
        IDX_INFO("getHealth: ok");
    } else {
        IDX_WARN("getHealth: %s", err.message);
    }

    char version[64];
    if (idx_rpc_get_version(rpc, version, sizeof(version), &err) == IDX_OK) {
        IDX_INFO("getVersion: solana-core %s", version);
    } else {
        IDX_ERROR("getVersion: %s", err.message);
        failures++;
    }

    /* --- slot and height --- */
    uint64_t slot = 0;
    if (idx_rpc_get_slot(rpc, "confirmed", &slot, &err) != IDX_OK) {
        IDX_ERROR("getSlot: %s", err.message);
        idx_rpc_close(rpc);
        return EXIT_FAILURE;
    }
    IDX_INFO("getSlot: %llu", (unsigned long long)slot);

    uint64_t height = 0;
    if (idx_rpc_get_block_height(rpc, "confirmed", &height, &err) == IDX_OK) {
        IDX_INFO("getBlockHeight: %llu (%llu slots skipped overall)",
                 (unsigned long long)height,
                 (unsigned long long)(slot - height));
    } else {
        IDX_ERROR("getBlockHeight: %s", err.message);
        failures++;
    }

    /* --- enumerating a range, which is how gaps get filled --- */
    idx_vec produced;
    idx_vec_init(&produced, sizeof(uint64_t));
    uint64_t from = slot - 20;
    if (idx_rpc_get_blocks(rpc, from, slot - 1, "confirmed", &produced, &err) ==
        IDX_OK) {
        size_t count = idx_vec_len(&produced);
        IDX_INFO("getBlocks(%llu..%llu): %zu of 20 slots produced a block",
                 (unsigned long long)from, (unsigned long long)(slot - 1),
                 count);

        /* Any slot in the range that getBlocks omitted was skipped, and
         * asking for it must report not-found rather than an error. */
        uint64_t skipped = 0;
        bool found_skipped = false;
        for (uint64_t candidate = from; candidate < slot && !found_skipped;
             candidate++) {
            bool present = false;
            for (size_t i = 0; i < count; i++) {
                if (*(uint64_t *)idx_vec_at(&produced, i) == candidate) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                skipped = candidate;
                found_skipped = true;
            }
        }

        if (found_skipped) {
            idx_rpc_response response;
            idx_status status =
                idx_rpc_get_block(rpc, skipped, NULL, &response, &err);
            if (status == IDX_ERR_NOT_FOUND) {
                IDX_INFO("getBlock(%llu): correctly reported as skipped — %s",
                         (unsigned long long)skipped, err.message);
            } else {
                IDX_ERROR("getBlock(%llu) on a skipped slot returned %s",
                          (unsigned long long)skipped,
                          idx_status_str(status));
                failures++;
                if (status == IDX_OK) {
                    idx_rpc_response_free(&response);
                }
            }
        } else {
            IDX_INFO("no skipped slot in the sampled range to probe");
        }
    } else {
        IDX_ERROR("getBlocks: %s", err.message);
        failures++;
    }

    /* --- a real block, which is where compression pays --- */
    uint64_t target = (idx_vec_len(&produced) > 0)
                          ? *(uint64_t *)idx_vec_at(&produced, 0)
                          : slot - 20;
    idx_rpc_response block;
    double started = now_seconds();
    if (idx_rpc_get_block(rpc, target, NULL, &block, &err) == IDX_OK) {
        double elapsed = now_seconds() - started;
        idx_json_val transactions = idx_json_get(block.result, "transactions");
        idx_slice blockhash = idx_slice_from_str("?");
        idx_json_opt_string(block.result, "blockhash", &blockhash);

        idx_rpc_stats stats;
        idx_rpc_get_stats(rpc, &stats);

        IDX_INFO("getBlock(%llu): %zu transactions, blockhash %.*s, %.2f s",
                 (unsigned long long)target,
                 idx_json_array_size(transactions), (int)blockhash.len,
                 (const char *)blockhash.data, elapsed);
        IDX_INFO("  decompressed to %.2f MiB total across all calls",
                 (double)stats.bytes_received / (1024.0 * 1024.0));
        idx_rpc_response_free(&block);
    } else {
        IDX_ERROR("getBlock: %s", err.message);
        failures++;
    }

    /* --- batching, which saves a round trip per call --- */
    idx_rpc_batch_call calls[3] = {
        {"getSlot", "[{\"commitment\":\"confirmed\"}]"},
        {"getBlockHeight", "[{\"commitment\":\"confirmed\"}]"},
        {"getVersion", "[]"},
    };
    idx_rpc_batch batch;
    if (idx_rpc_call_batch(rpc, calls, 3, &batch, &err) == IDX_OK) {
        int ok = 0;
        for (size_t i = 0; i < batch.count; i++) {
            if (batch.statuses[i] == IDX_OK) {
                ok++;
            } else {
                IDX_WARN("batch entry %zu (%s): %s", i, calls[i].method,
                         idx_status_str(batch.statuses[i]));
            }
        }
        IDX_INFO("batch of 3: %d succeeded in one round trip", ok);
        if (ok != 3) {
            failures++;
        }
        idx_rpc_batch_free(&batch);
    } else {
        IDX_ERROR("batch: %s", err.message);
        failures++;
    }

    idx_rpc_stats stats;
    idx_rpc_get_stats(rpc, &stats);
    IDX_INFO("requests=%llu retries=%llu rate_limited=%llu failovers=%llu",
             (unsigned long long)stats.requests,
             (unsigned long long)stats.retries,
             (unsigned long long)stats.rate_limited,
             (unsigned long long)stats.failovers);

    idx_vec_free(&produced);
    idx_rpc_close(rpc);

    if (failures > 0) {
        IDX_ERROR("%d check(s) failed", failures);
        return EXIT_FAILURE;
    }
    IDX_INFO("all checks passed");
    return EXIT_SUCCESS;
}
