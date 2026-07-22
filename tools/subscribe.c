/*
 * Exercises the PubSub layer against a live endpoint: subscribes, follows the
 * stream, and reports slot continuity so gaps and reconnections are visible.
 *
 *   ./build/debug/subscribe --count 10
 *   ./build/debug/subscribe --method slotSubscribe --params '[]' --count 20
 *
 * Runs until --count notifications arrive, or indefinitely with --count 0.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "pubsub.h"

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

static const char *k_default_params =
    "[\"all\",{\"commitment\":\"confirmed\",\"encoding\":\"json\","
    "\"transactionDetails\":\"full\",\"maxSupportedTransactionVersion\":0,"
    "\"showRewards\":false}]";

int main(int argc, char **argv) {
    const char *method = "blockSubscribe";
    const char *params = k_default_params;
    int wanted = 10;

    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) {
            fprintf(stderr, "option '%s' needs a value\n", argv[i]);
            return EXIT_FAILURE;
        }
        if (strcmp(argv[i], "--method") == 0) {
            method = argv[++i];
        } else if (strcmp(argv[i], "--params") == 0) {
            params = argv[++i];
        } else if (strcmp(argv[i], "--count") == 0) {
            wanted = atoi(argv[++i]);
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    const char *url = getenv("SOLANA_WSS_URL");
    if (url == NULL || *url == '\0') {
        fprintf(stderr, "SOLANA_WSS_URL is not set\n");
        return EXIT_FAILURE;
    }

    idx_log_init(stderr, IDX_LOG_DEBUG);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    idx_pubsub_options options;
    idx_pubsub_options_init(&options);
    options.url = url;

    idx_error err;
    idx_error_clear(&err);

    idx_pubsub *pubsub = NULL;
    if (idx_pubsub_open(&options, &pubsub, &err) != IDX_OK) {
        IDX_ERROR("%s", err.message);
        return EXIT_FAILURE;
    }

    uint64_t handle = 0;
    if (idx_pubsub_subscribe(pubsub, method, params, &handle, &err) != IDX_OK) {
        IDX_ERROR("%s", err.message);
        idx_pubsub_close(pubsub);
        return EXIT_FAILURE;
    }
    IDX_INFO("subscribed to %s as handle %llu", method,
             (unsigned long long)handle);

    int received = 0;
    int status = EXIT_SUCCESS;
    uint64_t previous_slot = 0;
    uint64_t gaps = 0;
    double started = now_seconds();

    while (!g_stop && (wanted == 0 || received < wanted)) {
        idx_pubsub_message message;
        idx_status code = idx_pubsub_poll(pubsub, 30000, &message, &err);

        if (code == IDX_ERR_TIMEOUT) {
            IDX_WARN("nothing for 30 s");
            continue;
        }
        if (code == IDX_ERR_REMOTE) {
            IDX_ERROR("%s", err.message);
            status = EXIT_FAILURE;
            break;
        }
        if (code != IDX_OK) {
            IDX_ERROR("%s", err.message);
            status = EXIT_FAILURE;
            break;
        }

        uint64_t slot = 0;
        if (!idx_json_opt_u64(idx_json_get(message.result, "context"), "slot",
                              &slot)) {
            idx_json_opt_u64(message.result, "slot", &slot);
        }

        const char *note = "";
        if (previous_slot != 0 && slot > previous_slot + 1) {
            gaps += slot - previous_slot - 1;
            note = "  <-- GAP";
        }
        previous_slot = slot;

        IDX_INFO("handle %llu: slot %llu, %.2f MiB%s",
                 (unsigned long long)message.subscription,
                 (unsigned long long)slot,
                 (double)message.raw.len / (1024.0 * 1024.0), note);

        idx_pubsub_message_free(&message);
        received++;
    }

    double elapsed = now_seconds() - started;
    idx_pubsub_stats stats;
    idx_pubsub_get_stats(pubsub, &stats);

    IDX_INFO("received=%d in %.1f s (%.2f/s) reconnects=%llu gaps=%llu "
             "unmatched=%llu failures=%llu",
             received, elapsed, (elapsed > 0) ? received / elapsed : 0.0,
             (unsigned long long)stats.reconnects, (unsigned long long)gaps,
             (unsigned long long)stats.unmatched,
             (unsigned long long)stats.subscribe_failures);

    idx_pubsub_close(pubsub);
    return status;
}
