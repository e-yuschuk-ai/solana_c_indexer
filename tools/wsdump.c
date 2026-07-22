/*
 * Subscribes over the WebSocket and reports what arrives.
 *
 * This exists to exercise the transport against a live endpoint, which unit
 * tests cannot do, and to capture real payloads as fixtures for the decoding
 * milestones.
 *
 *   ./build/debug/wsdump --count 3
 *   ./build/debug/wsdump --method slotSubscribe --params '[]' --count 5
 *   ./build/debug/wsdump --count 1 --save block.json
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "json.h"
#include "log.h"
#include "version.h"
#include "ws.h"

static const char *k_default_params =
    "[\"all\",{\"commitment\":\"confirmed\",\"encoding\":\"json\","
    "\"transactionDetails\":\"full\",\"maxSupportedTransactionVersion\":0,"
    "\"showRewards\":false}]";

typedef struct {
    const char *method;
    const char *params;
    const char *save_path;
    int count;
    int timeout_ms;
} options;

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "  --method NAME     subscription method (default blockSubscribe)\n"
            "  --params JSON     params array (default: all blocks, full)\n"
            "  --count N         messages to read before exiting (default 3)\n"
            "  --timeout MS      per-message timeout (default 60000)\n"
            "  --save PATH       write the first payload to PATH\n"
            "\n"
            "The endpoint comes from SOLANA_WSS_URL.\n",
            program);
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Pulls the slot out of whatever notification shape the method produces. */
static void describe(idx_slice message, char *out, size_t out_size) {
    idx_json_doc *doc = NULL;
    if (idx_json_parse(message, &doc, NULL) != IDX_OK) {
        snprintf(out, out_size, "unparseable");
        return;
    }

    idx_json_val root = idx_json_root(doc);
    idx_json_val result = idx_json_get(idx_json_get(root, "params"), "result");

    uint64_t slot = 0;
    if (idx_json_opt_u64(idx_json_get(result, "context"), "slot", &slot) ||
        idx_json_opt_u64(result, "slot", &slot)) {
        idx_json_val block = idx_json_get(idx_json_get(result, "value"), "block");
        size_t transactions =
            idx_json_array_size(idx_json_get(block, "transactions"));
        if (transactions > 0) {
            snprintf(out, out_size, "slot %llu, %zu transactions",
                     (unsigned long long)slot, transactions);
        } else {
            snprintf(out, out_size, "slot %llu", (unsigned long long)slot);
        }
    } else {
        uint64_t id = 0;
        if (idx_json_opt_u64(root, "id", &id)) {
            snprintf(out, out_size, "response to request %llu",
                     (unsigned long long)id);
        } else {
            snprintf(out, out_size, "unrecognized message");
        }
    }

    idx_json_free(doc);
}

static int parse_options(int argc, char **argv, options *out) {
    out->method = "blockSubscribe";
    out->params = k_default_params;
    out->save_path = NULL;
    out->count = 3;
    out->timeout_ms = 60000;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        int has_value = (i + 1 < argc);

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 1;
        }
        if (!has_value) {
            fprintf(stderr, "option '%s' needs a value\n", arg);
            return -1;
        }

        if (strcmp(arg, "--method") == 0) {
            out->method = argv[++i];
        } else if (strcmp(arg, "--params") == 0) {
            out->params = argv[++i];
        } else if (strcmp(arg, "--count") == 0) {
            out->count = atoi(argv[++i]);
        } else if (strcmp(arg, "--timeout") == 0) {
            out->timeout_ms = atoi(argv[++i]);
        } else if (strcmp(arg, "--save") == 0) {
            out->save_path = argv[++i];
        } else {
            fprintf(stderr, "unknown option '%s'\n", arg);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    idx_log_init(stderr, IDX_LOG_DEBUG);

    options opts;
    int parsed = parse_options(argc, argv, &opts);
    if (parsed != 0) {
        return (parsed > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    const char *url = getenv("SOLANA_WSS_URL");
    if (url == NULL || *url == '\0') {
        fprintf(stderr, "SOLANA_WSS_URL is not set\n");
        return EXIT_FAILURE;
    }

    idx_error err;
    idx_error_clear(&err);

    idx_ws_options ws_options;
    idx_ws_options_init(&ws_options);
    ws_options.url = url;

    idx_ws *ws = NULL;
    double started = now_seconds();
    if (idx_ws_connect(&ws_options, &ws, &err) != IDX_OK) {
        IDX_ERROR("%s", err.message);
        return EXIT_FAILURE;
    }
    IDX_INFO("connected in %.0f ms", (now_seconds() - started) * 1000.0);

    idx_buffer request;
    idx_buffer_init(&request);
    if (idx_json_write_rpc_request(&request, 1, opts.method, opts.params,
                                   &err) != IDX_OK) {
        IDX_ERROR("%s", err.message);
        idx_ws_close(ws);
        return EXIT_FAILURE;
    }

    if (idx_ws_send_text(ws, idx_buffer_slice(&request), &err) != IDX_OK) {
        IDX_ERROR("%s", err.message);
        idx_buffer_free(&request);
        idx_ws_close(ws);
        return EXIT_FAILURE;
    }
    IDX_INFO("sent %s", opts.method);
    idx_buffer_free(&request);

    int received = 0;
    int status = EXIT_SUCCESS;
    double previous = 0;

    /* The subscription confirmation counts as one message, so read one more. */
    while (received <= opts.count) {
        idx_slice message;
        double before = now_seconds();
        idx_status code = idx_ws_recv(ws, opts.timeout_ms, &message, &err);

        if (code == IDX_ERR_TIMEOUT) {
            IDX_WARN("timed out after %d ms", opts.timeout_ms);
            status = EXIT_FAILURE;
            break;
        }
        if (code != IDX_OK) {
            IDX_ERROR("%s", err.message);
            status = EXIT_FAILURE;
            break;
        }

        double now = now_seconds();
        char summary[128];
        describe(message, summary, sizeof(summary));

        if (received == 0) {
            IDX_INFO("subscribed: %.*s", (int)message.len,
                     (const char *)message.data);
        } else {
            IDX_INFO("message %d: %.2f MiB in %.0f ms (%+.2f s) — %s", received,
                     (double)message.len / (1024.0 * 1024.0),
                     (now - before) * 1000.0,
                     (previous > 0) ? now - previous : 0.0, summary);

            if (received == 1 && opts.save_path != NULL) {
                FILE *file = fopen(opts.save_path, "wb");
                if (file != NULL) {
                    fwrite(message.data, 1, message.len, file);
                    fclose(file);
                    IDX_INFO("saved %zu bytes to %s", message.len,
                             opts.save_path);
                } else {
                    IDX_WARN("cannot write %s", opts.save_path);
                }
            }
        }
        previous = now;
        received++;
    }

    idx_ws_stats stats;
    idx_ws_get_stats(ws, &stats);
    IDX_INFO("messages=%llu fragments=%llu bytes=%.2f MiB largest=%.2f MiB "
             "pings=%llu buffer=%.2f MiB",
             (unsigned long long)stats.messages_received,
             (unsigned long long)stats.fragments_received,
             (double)stats.bytes_received / (1024.0 * 1024.0),
             (double)stats.largest_message / (1024.0 * 1024.0),
             (unsigned long long)stats.pings_received,
             (double)idx_ws_buffer_capacity(ws) / (1024.0 * 1024.0));

    idx_ws_close(ws);
    return status;
}
