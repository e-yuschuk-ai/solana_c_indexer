/*
 * Follows the chain tip and reports slot continuity, so gaps and reconnections
 * are visible while developing. Exercises the whole transport stack: the
 * PubSub layer, the config-driven subscription shape, and the fallback from
 * blockSubscribe to slotSubscribe + getBlock.
 *
 *   ./build/debug/subscribe --count 10
 *   ./build/debug/subscribe --tx-details signatures --count 20
 *   ./build/debug/subscribe --block-filter <program> --count 5
 *
 * The subscription shape comes from the configuration (commitment, tx_details,
 * block_filter); the endpoints come from SOLANA_WSS_URL and SOLANA_RPC_URL.
 * Runs until --count notifications arrive, or indefinitely with --count 0.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "log.h"
#include "pubsub.h"
#include "rpc.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signal_number) {
    (void)signal_number;
    g_stop = 1;
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Tracks slot continuity across whichever source is delivering blocks. */
typedef struct {
    uint64_t previous_slot;
    uint64_t gaps;
    int received;
} progress;

static void note_slot(progress *p, uint64_t slot, double mib,
                      const char *source) {
    const char *gap_note = "";
    if (p->previous_slot != 0 && slot > p->previous_slot + 1) {
        p->gaps += slot - p->previous_slot - 1;
        gap_note = "  <-- GAP";
    }
    if (slot > p->previous_slot) {
        p->previous_slot = slot;
    }
    IDX_INFO("[%s] slot %llu, %.2f MiB%s", source, (unsigned long long)slot, mib,
             gap_note);
    p->received++;
}

/*
 * blockSubscribe path: whole blocks arrive on the socket. Returns
 * IDX_ERR_REMOTE if the endpoint rejects the subscription, which is the signal
 * to fall back.
 */
static idx_status run_block_subscribe(const idx_config *cfg, int wanted,
                                      progress *p, idx_error *err) {
    char params[IDX_CONFIG_STR_MAX + 128];
    IDX_TRY(idx_config_block_subscribe_params(cfg, params, sizeof(params), err));

    idx_pubsub_options options;
    idx_pubsub_options_init(&options);
    options.url = cfg->wss_url;

    idx_pubsub *pubsub = NULL;
    IDX_TRY(idx_pubsub_open(&options, &pubsub, err));

    uint64_t handle = 0;
    idx_status status = idx_pubsub_subscribe(
        pubsub, "blockSubscribe", "blockUnsubscribe", params, &handle, err);
    if (status != IDX_OK) {
        idx_pubsub_close(pubsub);
        return status;
    }
    IDX_INFO("subscribed to blockSubscribe (handle %llu)",
             (unsigned long long)handle);

    status = IDX_OK;
    while (!g_stop && (wanted == 0 || p->received < wanted)) {
        idx_pubsub_message message;
        idx_status code = idx_pubsub_poll(pubsub, 30000, &message, err);

        if (code == IDX_ERR_TIMEOUT) {
            IDX_WARN("nothing for 30 s");
            continue;
        }
        if (code != IDX_OK) {
            status = code; /* IDX_ERR_REMOTE triggers the fallback */
            break;
        }

        uint64_t slot = 0;
        idx_json_opt_u64(idx_json_get(message.result, "context"), "slot", &slot);
        note_slot(p, slot, (double)message.raw.len / (1024.0 * 1024.0),
                  "block");
        idx_pubsub_message_free(&message);
    }

    if (g_stop || status == IDX_OK) {
        idx_pubsub_unsubscribe(pubsub, handle, NULL);
    }
    idx_pubsub_close(pubsub);
    return status;
}

/*
 * Fallback path: slotSubscribe announces each slot, and the block itself is
 * fetched over HTTP. This is what runs against an endpoint that does not offer
 * blockSubscribe.
 */
static idx_status run_slot_fallback(const idx_config *cfg, int wanted,
                                    progress *p, idx_error *err) {
    if (cfg->rpc_url[0] == '\0') {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "fallback needs an RPC endpoint (SOLANA_RPC_URL)");
    }
    IDX_WARN("falling back to slotSubscribe + getBlock");

    const char *urls[] = {cfg->rpc_url};
    idx_rpc_options rpc_options;
    idx_rpc_options_init(&rpc_options);
    rpc_options.urls = urls;
    rpc_options.url_count = 1;
    rpc_options.blocks_range_limit = cfg->blocks_range_limit;

    idx_rpc *rpc = NULL;
    IDX_TRY(idx_rpc_open(&rpc_options, &rpc, err));

    idx_pubsub_options options;
    idx_pubsub_options_init(&options);
    options.url = cfg->wss_url;

    idx_pubsub *pubsub = NULL;
    idx_status status = idx_pubsub_open(&options, &pubsub, err);
    if (status != IDX_OK) {
        idx_rpc_close(rpc);
        return status;
    }

    uint64_t handle = 0;
    status = idx_pubsub_subscribe(pubsub, "slotSubscribe", "slotUnsubscribe",
                                  "[]", &handle, err);
    if (status != IDX_OK) {
        idx_pubsub_close(pubsub);
        idx_rpc_close(rpc);
        return status;
    }
    IDX_INFO("subscribed to slotSubscribe (handle %llu)",
             (unsigned long long)handle);

    idx_rpc_block_options block_options;
    idx_rpc_block_options_init(&block_options);
    block_options.commitment = cfg->commitment;
    block_options.transaction_details = idx_tx_details_name(cfg->tx_details);

    status = IDX_OK;
    while (!g_stop && (wanted == 0 || p->received < wanted)) {
        idx_pubsub_message message;
        idx_status code = idx_pubsub_poll(pubsub, 30000, &message, err);
        if (code == IDX_ERR_TIMEOUT) {
            continue;
        }
        if (code != IDX_OK) {
            status = code;
            break;
        }

        uint64_t slot = 0;
        idx_json_opt_u64(message.result, "slot", &slot);
        idx_pubsub_message_free(&message);
        if (slot == 0) {
            continue;
        }

        idx_rpc_response block;
        idx_error fetch_err;
        idx_error_clear(&fetch_err);
        idx_status fetched =
            idx_rpc_get_block(rpc, slot, &block_options, &block, &fetch_err);

        if (fetched == IDX_ERR_NOT_FOUND) {
            IDX_DEBUG("slot %llu was skipped", (unsigned long long)slot);
            continue;
        }
        if (fetched != IDX_OK) {
            IDX_WARN("getBlock(%llu): %s", (unsigned long long)slot,
                     fetch_err.message);
            continue;
        }

        idx_rpc_stats rpc_stats;
        idx_rpc_get_stats(rpc, &rpc_stats);
        note_slot(p, slot, 0.0, "slot+rpc");
        idx_rpc_response_free(&block);
    }

    if (g_stop || status == IDX_OK) {
        idx_pubsub_unsubscribe(pubsub, handle, NULL);
    }
    idx_pubsub_close(pubsub);
    idx_rpc_close(rpc);
    return status;
}

int main(int argc, char **argv) {
    idx_log_init(stderr, IDX_LOG_DEBUG);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /*
     * --count is ours; everything else is handed to the config loader, so
     * --tx-details, --block-filter and --commitment all work here. The count
     * flag and its value are stripped from the vector first.
     */
    int wanted = 10;
    char *filtered[64];
    int filtered_argc = 0;
    for (int i = 0; i < argc && filtered_argc < 64; i++) {
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            wanted = atoi(argv[++i]);
            continue;
        }
        filtered[filtered_argc++] = argv[i];
    }

    idx_config cfg;
    idx_error err;
    idx_error_clear(&err);
    if (idx_config_load(&cfg, filtered_argc, filtered, &err) != IDX_OK) {
        fprintf(stderr, "configuration error: %s\n", err.message);
        return EXIT_FAILURE;
    }

    if (cfg.wss_url[0] == '\0') {
        fprintf(stderr, "SOLANA_WSS_URL is not set\n");
        return EXIT_FAILURE;
    }

    idx_log_set_level(cfg.log_level);
    IDX_INFO("commitment=%s tx_details=%s filter=%s", cfg.commitment,
             idx_tx_details_name(cfg.tx_details), cfg.block_filter);

    progress p;
    memset(&p, 0, sizeof(p));
    double started = now_seconds();

    idx_status status = run_block_subscribe(&cfg, wanted, &p, &err);

    if (status == IDX_ERR_REMOTE) {
        IDX_WARN("blockSubscribe unavailable: %s", err.message);
        idx_error_clear(&err);
        status = run_slot_fallback(&cfg, wanted, &p, &err);
    }

    double elapsed = now_seconds() - started;
    IDX_INFO("received=%d in %.1f s (%.2f/s) gaps=%llu", p.received, elapsed,
             (elapsed > 0) ? p.received / elapsed : 0.0,
             (unsigned long long)p.gaps);

    if (status != IDX_OK && !g_stop) {
        IDX_ERROR("%s", err.message);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
