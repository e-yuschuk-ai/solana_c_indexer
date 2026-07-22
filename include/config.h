/*
 * Runtime configuration.
 *
 * Values are resolved from four sources, each overriding the previous one:
 *   1. built-in defaults
 *   2. a configuration file (--config, or ./indexer.conf when present)
 *   3. environment variables
 *   4. command line flags
 *
 * All strings are fixed-size buffers so an idx_config can be copied and
 * discarded freely without ownership rules.
 */
#ifndef IDX_CONFIG_H
#define IDX_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "error.h"
#include "log.h"

#define IDX_CONFIG_STR_MAX 512
#define IDX_CONFIG_MAX_CONCURRENCY 1024u

/* How much of each transaction blockSubscribe/getBlock returns. */
typedef enum {
    IDX_TX_DETAILS_FULL = 0,
    IDX_TX_DETAILS_ACCOUNTS,
    IDX_TX_DETAILS_SIGNATURES,
    IDX_TX_DETAILS_NONE
} idx_tx_details;

typedef struct {
    char rpc_url[IDX_CONFIG_STR_MAX];
    char wss_url[IDX_CONFIG_STR_MAX];
    char config_file[IDX_CONFIG_STR_MAX];
    idx_log_level log_level;
    uint64_t start_slot; /* 0 = start from the current chain tip */
    uint64_t end_slot;   /* 0 = follow the tip indefinitely */
    uint32_t concurrency;

    /*
     * Subscription shape. The default — follow every block at full detail — is
     * what a general-purpose indexer wants; the filter and detail level are the
     * first levers for a deployment that only cares about specific programs
     * (decision D1a).
     */
    char commitment[32];            /* "confirmed" (default) or "finalized" */
    idx_tx_details tx_details;      /* full by default */
    char block_filter[IDX_CONFIG_STR_MAX]; /* "all", or a program/account key */

    /* Largest slot span getBlocks may be asked for in one call; providers cap
     * this per plan. 0 selects the client's default. */
    uint64_t blocks_range_limit;

    bool help; /* set when --help was requested; not a config value */
} idx_config;

/* Lowercase name ("full"); never returns NULL. */
const char *idx_tx_details_name(idx_tx_details details);

/* Parses "full"/"accounts"/"signatures"/"none", case-insensitively. */
idx_status idx_tx_details_parse(const char *name, idx_tx_details *out);

/* Fills `cfg` with built-in defaults. */
void idx_config_defaults(idx_config *cfg);

/* Applies all four sources in precedence order. */
idx_status idx_config_load(idx_config *cfg, int argc, char **argv,
                           idx_error *err);

/*
 * Individual sources, exposed for testing and for callers that build a
 * configuration programmatically.
 */
idx_status idx_config_apply_file(idx_config *cfg, const char *path,
                                 idx_error *err);
idx_status idx_config_apply_env(idx_config *cfg, idx_error *err);
idx_status idx_config_apply_args(idx_config *cfg, int argc, char **argv,
                                 idx_error *err);

/* Applies a single `key = value` pair. `source` only appears in messages. */
idx_status idx_config_apply_kv(idx_config *cfg, const char *key,
                               const char *value, const char *source,
                               idx_error *err);

/* Rejects configurations that are internally inconsistent or incomplete. */
idx_status idx_config_validate(const idx_config *cfg, idx_error *err);

/*
 * Writes the `params` array for a blockSubscribe request built from the
 * subscription fields: the filter (`"all"` or `{"mentionsAccountOrProgram":
 * "<key>"}`) followed by the encoding, commitment and detail options. The
 * result is valid JSON suitable for idx_json_write_rpc_request.
 */
idx_status idx_config_block_subscribe_params(const idx_config *cfg, char *out,
                                             size_t out_size, idx_error *err);

/* Writes the effective configuration at INFO level. */
void idx_config_log(const idx_config *cfg);

void idx_config_usage(FILE *out, const char *program);

#endif /* IDX_CONFIG_H */
