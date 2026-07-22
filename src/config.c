#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define IDX_CONFIG_DEFAULT_FILE "indexer.conf"
#define IDX_CONFIG_LINE_MAX 1024

static const char *const k_tx_details_names[] = {"full", "accounts",
                                                 "signatures", "none"};
static const size_t k_tx_details_count =
    sizeof(k_tx_details_names) / sizeof(k_tx_details_names[0]);

const char *idx_tx_details_name(idx_tx_details details) {
    size_t index = (size_t)details;
    if (index >= k_tx_details_count) {
        return "unknown";
    }
    return k_tx_details_names[index];
}

idx_status idx_tx_details_parse(const char *name, idx_tx_details *out) {
    if (name == NULL || out == NULL) {
        return IDX_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < k_tx_details_count; i++) {
        if (strcasecmp(name, k_tx_details_names[i]) == 0) {
            *out = (idx_tx_details)i;
            return IDX_OK;
        }
    }
    return IDX_ERR_PARSE;
}

void idx_config_defaults(idx_config *cfg) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->log_level = IDX_LOG_INFO;
    cfg->start_slot = 0;
    cfg->end_slot = 0;
    cfg->concurrency = 4;
    snprintf(cfg->commitment, sizeof(cfg->commitment), "confirmed");
    cfg->tx_details = IDX_TX_DETAILS_FULL;
    snprintf(cfg->block_filter, sizeof(cfg->block_filter), "all");
    cfg->blocks_range_limit = 0;
    cfg->help = false;
}

static idx_status set_string(char *dest, size_t dest_size, const char *value,
                             const char *key, const char *source,
                             idx_error *err) {
    size_t len = strlen(value);
    if (len >= dest_size) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "%s: %s is %zu bytes, maximum is %zu", source, key, len,
                        dest_size - 1);
    }
    memcpy(dest, value, len + 1);
    return IDX_OK;
}

static idx_status parse_u64(const char *value, uint64_t *out, const char *key,
                            const char *source, idx_error *err) {
    if (*value == '\0') {
        return IDX_FAIL(err, IDX_ERR_PARSE, "%s: %s is empty", source, key);
    }

    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno == ERANGE || parsed > UINT64_MAX) {
        return IDX_FAIL(err, IDX_ERR_RANGE, "%s: %s = %s is out of range",
                        source, key, value);
    }
    if (end == value || *end != '\0') {
        return IDX_FAIL(err, IDX_ERR_PARSE, "%s: %s = %s is not a number",
                        source, key, value);
    }
    /* strtoull accepts a leading '-' and wraps around; reject it explicitly. */
    if (strchr(value, '-') != NULL) {
        return IDX_FAIL(err, IDX_ERR_RANGE, "%s: %s = %s must not be negative",
                        source, key, value);
    }

    *out = (uint64_t)parsed;
    return IDX_OK;
}

idx_status idx_config_apply_kv(idx_config *cfg, const char *key,
                               const char *value, const char *source,
                               idx_error *err) {
    if (cfg == NULL || key == NULL || value == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "cfg, key and value must not be NULL");
    }
    if (source == NULL) {
        source = "config";
    }

    if (strcmp(key, "rpc_url") == 0) {
        return set_string(cfg->rpc_url, sizeof(cfg->rpc_url), value, key, source,
                          err);
    }
    if (strcmp(key, "wss_url") == 0) {
        return set_string(cfg->wss_url, sizeof(cfg->wss_url), value, key, source,
                          err);
    }
    if (strcmp(key, "log_level") == 0) {
        idx_log_level level;
        if (idx_log_level_parse(value, &level) != IDX_OK) {
            return IDX_FAIL(err, IDX_ERR_PARSE,
                            "%s: log_level = %s is not one of "
                            "error/warn/info/debug/trace",
                            source, value);
        }
        cfg->log_level = level;
        return IDX_OK;
    }
    if (strcmp(key, "start_slot") == 0) {
        return parse_u64(value, &cfg->start_slot, key, source, err);
    }
    if (strcmp(key, "end_slot") == 0) {
        return parse_u64(value, &cfg->end_slot, key, source, err);
    }
    if (strcmp(key, "concurrency") == 0) {
        uint64_t parsed = 0;
        IDX_TRY(parse_u64(value, &parsed, key, source, err));
        if (parsed == 0 || parsed > IDX_CONFIG_MAX_CONCURRENCY) {
            return IDX_FAIL(err, IDX_ERR_RANGE,
                            "%s: concurrency = %s must be between 1 and %u",
                            source, value, IDX_CONFIG_MAX_CONCURRENCY);
        }
        cfg->concurrency = (uint32_t)parsed;
        return IDX_OK;
    }
    if (strcmp(key, "commitment") == 0) {
        if (strcmp(value, "confirmed") != 0 && strcmp(value, "finalized") != 0) {
            return IDX_FAIL(err, IDX_ERR_PARSE,
                            "%s: commitment = %s must be confirmed or finalized",
                            source, value);
        }
        return set_string(cfg->commitment, sizeof(cfg->commitment), value, key,
                          source, err);
    }
    if (strcmp(key, "tx_details") == 0 ||
        strcmp(key, "transaction_details") == 0) {
        idx_tx_details details;
        if (idx_tx_details_parse(value, &details) != IDX_OK) {
            return IDX_FAIL(err, IDX_ERR_PARSE,
                            "%s: tx_details = %s is not one of "
                            "full/accounts/signatures/none",
                            source, value);
        }
        cfg->tx_details = details;
        return IDX_OK;
    }
    if (strcmp(key, "block_filter") == 0) {
        return set_string(cfg->block_filter, sizeof(cfg->block_filter), value,
                          key, source, err);
    }
    if (strcmp(key, "blocks_range_limit") == 0) {
        return parse_u64(value, &cfg->blocks_range_limit, key, source, err);
    }
    if (strcmp(key, "config") == 0 || strcmp(key, "config_file") == 0) {
        return set_string(cfg->config_file, sizeof(cfg->config_file), value,
                          key, source, err);
    }

    return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "%s: unknown setting '%s'", source,
                    key);
}

/* Trims ASCII whitespace in place and returns the first non-space character. */
static char *trim(char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    size_t len = strlen(text);
    while (len > 0) {
        char last = text[len - 1];
        if (last != ' ' && last != '\t' && last != '\r' && last != '\n') {
            break;
        }
        text[--len] = '\0';
    }
    return text;
}

idx_status idx_config_apply_file(idx_config *cfg, const char *path,
                                 idx_error *err) {
    if (cfg == NULL || path == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "cfg and path must not be NULL");
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return IDX_FAIL(err, IDX_ERR_IO, "cannot open config file '%s': %s",
                        path, strerror(errno));
    }

    char line[IDX_CONFIG_LINE_MAX];
    int line_number = 0;
    idx_status status = IDX_OK;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;

        if (strchr(line, '\n') == NULL && !feof(file)) {
            status = IDX_FAIL(err, IDX_ERR_PARSE, "%s:%d: line exceeds %d bytes",
                              path, line_number, IDX_CONFIG_LINE_MAX - 1);
            break;
        }

        char *comment = strchr(line, '#');
        if (comment != NULL) {
            *comment = '\0';
        }

        char *content = trim(line);
        if (*content == '\0') {
            continue;
        }

        char *separator = strchr(content, '=');
        if (separator == NULL) {
            status = IDX_FAIL(err, IDX_ERR_PARSE,
                              "%s:%d: expected 'key = value'", path,
                              line_number);
            break;
        }
        *separator = '\0';

        char *key = trim(content);
        char *value = trim(separator + 1);

        char source[IDX_CONFIG_STR_MAX];
        snprintf(source, sizeof(source), "%s:%d", path, line_number);

        status = idx_config_apply_kv(cfg, key, value, source, err);
        if (status != IDX_OK) {
            break;
        }
    }

    if (status == IDX_OK && ferror(file)) {
        status = IDX_FAIL(err, IDX_ERR_IO, "error reading '%s': %s", path,
                          strerror(errno));
    }

    fclose(file);
    return status;
}

idx_status idx_config_apply_env(idx_config *cfg, idx_error *err) {
    static const struct {
        const char *variable;
        const char *key;
    } mapping[] = {
        {"SOLANA_RPC_URL", "rpc_url"},
        {"SOLANA_WSS_URL", "wss_url"},
        {"INDEXER_LOG_LEVEL", "log_level"},
        {"INDEXER_START_SLOT", "start_slot"},
        {"INDEXER_END_SLOT", "end_slot"},
        {"INDEXER_CONCURRENCY", "concurrency"},
        {"INDEXER_COMMITMENT", "commitment"},
        {"INDEXER_TX_DETAILS", "tx_details"},
        {"INDEXER_BLOCK_FILTER", "block_filter"},
        {"INDEXER_BLOCKS_RANGE_LIMIT", "blocks_range_limit"},
    };

    if (cfg == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "cfg must not be NULL");
    }

    for (size_t i = 0; i < sizeof(mapping) / sizeof(mapping[0]); i++) {
        const char *value = getenv(mapping[i].variable);
        if (value == NULL || *value == '\0') {
            continue;
        }
        IDX_TRY(idx_config_apply_kv(cfg, mapping[i].key, value,
                                    mapping[i].variable, err));
    }
    return IDX_OK;
}

/* Converts "--start-slot" into "start_slot". Returns 0 if it does not fit. */
static int flag_to_key(const char *flag, char *out, size_t out_size) {
    if (flag[0] != '-' || flag[1] != '-') {
        return 0;
    }
    flag += 2;

    size_t len = strlen(flag);
    if (len == 0 || len >= out_size) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = (flag[i] == '-') ? '_' : flag[i];
    }
    out[len] = '\0';
    return 1;
}

idx_status idx_config_apply_args(idx_config *cfg, int argc, char **argv,
                                 idx_error *err) {
    if (cfg == NULL || (argc > 0 && argv == NULL)) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "cfg and argv must not be NULL");
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            cfg->help = true;
            return IDX_OK;
        }

        char key[64];
        const char *value = NULL;

        /* Accept both "--key value" and "--key=value". */
        const char *equals = strchr(arg, '=');
        if (equals != NULL) {
            char flag[64];
            size_t flag_len = (size_t)(equals - arg);
            if (flag_len >= sizeof(flag)) {
                return IDX_FAIL(err, IDX_ERR_PARSE, "unknown option '%s'", arg);
            }
            memcpy(flag, arg, flag_len);
            flag[flag_len] = '\0';
            if (!flag_to_key(flag, key, sizeof(key))) {
                return IDX_FAIL(err, IDX_ERR_PARSE, "unknown option '%s'", arg);
            }
            value = equals + 1;
        } else {
            if (strcmp(arg, "-c") == 0) {
                snprintf(key, sizeof(key), "config");
            } else if (!flag_to_key(arg, key, sizeof(key))) {
                return IDX_FAIL(err, IDX_ERR_PARSE, "unexpected argument '%s'",
                                arg);
            }
            if (i + 1 >= argc) {
                return IDX_FAIL(err, IDX_ERR_PARSE, "option '%s' needs a value",
                                arg);
            }
            value = argv[++i];
        }

        IDX_TRY(idx_config_apply_kv(cfg, key, value, "command line", err));
    }
    return IDX_OK;
}

/*
 * Pre-scan for --config so the file it names is loaded before the environment
 * and the remaining flags, which must still be able to override it.
 */
static idx_status find_config_file(idx_config *cfg, int argc, char **argv,
                                   idx_error *err) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;

        if (strcmp(arg, "--config") == 0 || strcmp(arg, "-c") == 0) {
            if (i + 1 >= argc) {
                return IDX_FAIL(err, IDX_ERR_PARSE, "option '%s' needs a value",
                                arg);
            }
            value = argv[i + 1];
        } else if (strncmp(arg, "--config=", 9) == 0) {
            value = arg + 9;
        }

        if (value != NULL) {
            return set_string(cfg->config_file, sizeof(cfg->config_file), value,
                              "config", "command line", err);
        }
    }
    return IDX_OK;
}

idx_status idx_config_load(idx_config *cfg, int argc, char **argv,
                           idx_error *err) {
    if (cfg == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "cfg must not be NULL");
    }

    idx_config_defaults(cfg);

    /* --help is answered before anything else can fail. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            cfg->help = true;
            return IDX_OK;
        }
    }

    IDX_TRY(find_config_file(cfg, argc, argv, err));

    if (cfg->config_file[0] != '\0') {
        /* An explicitly requested file must exist. */
        IDX_TRY(idx_config_apply_file(cfg, cfg->config_file, err));
    } else {
        FILE *probe = fopen(IDX_CONFIG_DEFAULT_FILE, "r");
        if (probe != NULL) {
            fclose(probe);
            IDX_TRY(set_string(cfg->config_file, sizeof(cfg->config_file),
                               IDX_CONFIG_DEFAULT_FILE, "config", "default",
                               err));
            IDX_TRY(idx_config_apply_file(cfg, IDX_CONFIG_DEFAULT_FILE, err));
        }
    }

    IDX_TRY(idx_config_apply_env(cfg, err));
    IDX_TRY(idx_config_apply_args(cfg, argc, argv, err));
    return IDX_OK;
}

idx_status idx_config_validate(const idx_config *cfg, idx_error *err) {
    if (cfg == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "cfg must not be NULL");
    }
    if (cfg->rpc_url[0] == '\0') {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "rpc_url is required (set SOLANA_RPC_URL or --rpc-url)");
    }
    if (strncmp(cfg->rpc_url, "http://", 7) != 0 &&
        strncmp(cfg->rpc_url, "https://", 8) != 0) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "rpc_url must start with http:// or https://");
    }
    if (cfg->wss_url[0] != '\0' && strncmp(cfg->wss_url, "ws://", 5) != 0 &&
        strncmp(cfg->wss_url, "wss://", 6) != 0) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "wss_url must start with ws:// or wss://");
    }
    if (cfg->end_slot != 0 && cfg->end_slot < cfg->start_slot) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "end_slot (%" PRIu64 ") is before start_slot (%" PRIu64
                        ")",
                        cfg->end_slot, cfg->start_slot);
    }
    if (cfg->concurrency == 0 ||
        cfg->concurrency > IDX_CONFIG_MAX_CONCURRENCY) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "concurrency must be between 1 and %u",
                        IDX_CONFIG_MAX_CONCURRENCY);
    }
    if (cfg->commitment[0] == '\0') {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "commitment must be set");
    }
    if (cfg->block_filter[0] == '\0') {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "block_filter must be 'all' or a base58 key");
    }
    return IDX_OK;
}

idx_status idx_config_block_subscribe_params(const idx_config *cfg, char *out,
                                             size_t out_size, idx_error *err) {
    if (cfg == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "cfg and out must not be NULL");
    }

    /* "all" is a bare string; a key becomes a mentions filter object. */
    char filter[IDX_CONFIG_STR_MAX + 40];
    if (strcmp(cfg->block_filter, "all") == 0) {
        snprintf(filter, sizeof(filter), "\"all\"");
    } else {
        snprintf(filter, sizeof(filter),
                 "{\"mentionsAccountOrProgram\":\"%s\"}", cfg->block_filter);
    }

    int written = snprintf(
        out, out_size,
        "[%s,{\"commitment\":\"%s\",\"encoding\":\"json\","
        "\"transactionDetails\":\"%s\",\"maxSupportedTransactionVersion\":0,"
        "\"showRewards\":false}]",
        filter, cfg->commitment, idx_tx_details_name(cfg->tx_details));

    if (written < 0 || (size_t)written >= out_size) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "blockSubscribe params need %d bytes, got %zu",
                        written, out_size);
    }
    return IDX_OK;
}

void idx_config_log(const idx_config *cfg) {
    if (cfg == NULL) {
        return;
    }
    IDX_INFO("config: rpc_url = %s", cfg->rpc_url);
    IDX_INFO("config: wss_url = %s",
             (cfg->wss_url[0] != '\0') ? cfg->wss_url : "<unset>");
    IDX_INFO("config: log_level = %s", idx_log_level_name(cfg->log_level));
    IDX_INFO("config: start_slot = %" PRIu64 "%s", cfg->start_slot,
             (cfg->start_slot == 0) ? " (chain tip)" : "");
    IDX_INFO("config: end_slot = %" PRIu64 "%s", cfg->end_slot,
             (cfg->end_slot == 0) ? " (follow)" : "");
    IDX_INFO("config: concurrency = %" PRIu32, cfg->concurrency);
    IDX_INFO("config: commitment = %s", cfg->commitment);
    IDX_INFO("config: tx_details = %s", idx_tx_details_name(cfg->tx_details));
    IDX_INFO("config: block_filter = %s", cfg->block_filter);
    if (cfg->blocks_range_limit != 0) {
        IDX_INFO("config: blocks_range_limit = %" PRIu64,
                 cfg->blocks_range_limit);
    }
    if (cfg->config_file[0] != '\0') {
        IDX_INFO("config: loaded from %s", cfg->config_file);
    }
}

void idx_config_usage(FILE *out, const char *program) {
    fprintf(out,
            "Usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  -c, --config PATH      Configuration file (default: %s)\n"
            "      --rpc-url URL      Solana JSON-RPC endpoint (required)\n"
            "      --wss-url URL      Solana WebSocket endpoint\n"
            "      --log-level LEVEL  error, warn, info, debug or trace\n"
            "      --start-slot N     First slot to index (0 = chain tip)\n"
            "      --end-slot N       Last slot to index (0 = follow the tip)\n"
            "      --concurrency N    Parallel fetchers (1-%u)\n"
            "      --commitment C     confirmed (default) or finalized\n"
            "      --tx-details D     full, accounts, signatures or none\n"
            "      --block-filter F   'all' (default) or a program/account key\n"
            "      --blocks-range-limit N  getBlocks span cap for the plan\n"
            "  -h, --help             Show this message\n"
            "\n"
            "Environment:\n"
            "  SOLANA_RPC_URL, SOLANA_WSS_URL, INDEXER_LOG_LEVEL,\n"
            "  INDEXER_START_SLOT, INDEXER_END_SLOT, INDEXER_CONCURRENCY,\n"
            "  INDEXER_COMMITMENT, INDEXER_TX_DETAILS, INDEXER_BLOCK_FILTER,\n"
            "  INDEXER_BLOCKS_RANGE_LIMIT\n"
            "\n"
            "Precedence: defaults < config file < environment < command line\n",
            (program != NULL) ? program : "indexer", IDX_CONFIG_DEFAULT_FILE,
            IDX_CONFIG_MAX_CONCURRENCY);
}
