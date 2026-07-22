#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "test.h"

static void test_defaults(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);

    TEST_EQ_STR(cfg.rpc_url, "");
    TEST_EQ_INT(cfg.log_level, IDX_LOG_INFO);
    TEST_EQ_UINT(cfg.start_slot, 0u);
    TEST_EQ_UINT(cfg.end_slot, 0u);
    TEST_EQ_UINT(cfg.concurrency, 4u);
    TEST_EQ_STR(cfg.commitment, "confirmed");
    TEST_EQ_INT(cfg.tx_details, IDX_TX_DETAILS_FULL);
    TEST_EQ_STR(cfg.block_filter, "all");
    TEST_EQ_UINT(cfg.blocks_range_limit, 0u);
    TEST_ASSERT(!cfg.help);
}

static void test_subscription_settings(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);

    TEST_EQ_INT(idx_config_apply_kv(&cfg, "commitment", "finalized", "t", NULL),
                IDX_OK);
    TEST_EQ_STR(cfg.commitment, "finalized");
    TEST_EQ_INT(idx_config_apply_kv(&cfg, "commitment", "processed", "t", NULL),
                IDX_ERR_PARSE);

    TEST_EQ_INT(idx_config_apply_kv(&cfg, "tx_details", "signatures", "t", NULL),
                IDX_OK);
    TEST_EQ_INT(cfg.tx_details, IDX_TX_DETAILS_SIGNATURES);
    /* The RPC alias must work too. */
    TEST_EQ_INT(
        idx_config_apply_kv(&cfg, "transaction_details", "none", "t", NULL),
        IDX_OK);
    TEST_EQ_INT(cfg.tx_details, IDX_TX_DETAILS_NONE);
    TEST_EQ_INT(idx_config_apply_kv(&cfg, "tx_details", "verbose", "t", NULL),
                IDX_ERR_PARSE);

    TEST_EQ_INT(idx_config_apply_kv(&cfg, "block_filter",
                                    "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
                                    "t", NULL),
                IDX_OK);
    TEST_EQ_STR(cfg.block_filter,
                "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA");

    TEST_EQ_INT(
        idx_config_apply_kv(&cfg, "blocks_range_limit", "5", "t", NULL), IDX_OK);
    TEST_EQ_UINT(cfg.blocks_range_limit, 5u);
}

static void test_tx_details_names(void) {
    TEST_EQ_STR(idx_tx_details_name(IDX_TX_DETAILS_FULL), "full");
    TEST_EQ_STR(idx_tx_details_name(IDX_TX_DETAILS_ACCOUNTS), "accounts");
    TEST_EQ_STR(idx_tx_details_name(IDX_TX_DETAILS_SIGNATURES), "signatures");
    TEST_EQ_STR(idx_tx_details_name(IDX_TX_DETAILS_NONE), "none");
    TEST_EQ_STR(idx_tx_details_name((idx_tx_details)99), "unknown");

    idx_tx_details details;
    TEST_EQ_INT(idx_tx_details_parse("FULL", &details), IDX_OK);
    TEST_EQ_INT(details, IDX_TX_DETAILS_FULL);
    TEST_EQ_INT(idx_tx_details_parse("nope", &details), IDX_ERR_PARSE);
    TEST_EQ_INT(idx_tx_details_parse(NULL, &details), IDX_ERR_INVALID_ARG);
}

/* The generated params must be valid JSON and carry the config's choices. */
static void test_block_subscribe_params(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);

    char params[IDX_CONFIG_STR_MAX + 128];
    idx_error err;
    idx_error_clear(&err);

    /* Default: filter is the bare string "all". */
    TEST_EQ_INT(idx_config_block_subscribe_params(&cfg, params, sizeof(params),
                                                  &err),
                IDX_OK);
    TEST_ASSERT(strstr(params, "[\"all\",{") == params);
    TEST_ASSERT(strstr(params, "\"commitment\":\"confirmed\"") != NULL);
    TEST_ASSERT(strstr(params, "\"transactionDetails\":\"full\"") != NULL);

    /* A key becomes a mentions filter object. */
    idx_config_apply_kv(&cfg, "block_filter", "Vote111111111111111111111111111",
                        "t", NULL);
    idx_config_apply_kv(&cfg, "tx_details", "accounts", "t", NULL);
    idx_config_apply_kv(&cfg, "commitment", "finalized", "t", NULL);
    TEST_EQ_INT(idx_config_block_subscribe_params(&cfg, params, sizeof(params),
                                                  &err),
                IDX_OK);
    TEST_ASSERT(strstr(params,
                       "{\"mentionsAccountOrProgram\":"
                       "\"Vote111111111111111111111111111\"}") != NULL);
    TEST_ASSERT(strstr(params, "\"transactionDetails\":\"accounts\"") != NULL);
    TEST_ASSERT(strstr(params, "\"commitment\":\"finalized\"") != NULL);

    /* A buffer too small must be reported, not overrun. */
    char tiny[16];
    TEST_EQ_INT(
        idx_config_block_subscribe_params(&cfg, tiny, sizeof(tiny), NULL),
        IDX_ERR_RANGE);
}

static void test_apply_kv(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);

    TEST_EQ_INT(idx_config_apply_kv(&cfg, "rpc_url", "https://rpc.example/x",
                                    "test", NULL),
                IDX_OK);
    TEST_EQ_STR(cfg.rpc_url, "https://rpc.example/x");

    TEST_EQ_INT(idx_config_apply_kv(&cfg, "log_level", "DEBUG", "test", NULL),
                IDX_OK);
    TEST_EQ_INT(cfg.log_level, IDX_LOG_DEBUG);

    TEST_EQ_INT(idx_config_apply_kv(&cfg, "start_slot", "250000000", "test",
                                    NULL),
                IDX_OK);
    TEST_EQ_UINT(cfg.start_slot, 250000000u);

    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_config_apply_kv(&cfg, "nope", "1", "test", &err),
                IDX_ERR_NOT_FOUND);
    TEST_ASSERT(strstr(err.message, "nope") != NULL);

    TEST_EQ_INT(idx_config_apply_kv(&cfg, "start_slot", "abc", "test", NULL),
                IDX_ERR_PARSE);
    TEST_EQ_INT(idx_config_apply_kv(&cfg, "start_slot", "-5", "test", NULL),
                IDX_ERR_RANGE);
    TEST_EQ_INT(idx_config_apply_kv(&cfg, "log_level", "loud", "test", NULL),
                IDX_ERR_PARSE);
    TEST_EQ_INT(idx_config_apply_kv(&cfg, "concurrency", "0", "test", NULL),
                IDX_ERR_RANGE);
    TEST_EQ_INT(idx_config_apply_kv(&cfg, "concurrency", "99999", "test", NULL),
                IDX_ERR_RANGE);
}

static void test_string_too_long(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);

    char long_url[IDX_CONFIG_STR_MAX + 16];
    memset(long_url, 'a', sizeof(long_url) - 1);
    long_url[sizeof(long_url) - 1] = '\0';

    TEST_EQ_INT(idx_config_apply_kv(&cfg, "rpc_url", long_url, "test", NULL),
                IDX_ERR_RANGE);
    TEST_EQ_STR(cfg.rpc_url, ""); /* rejected without partial writes */
}

static void test_apply_args(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);

    char *argv[] = {"indexer",     "--rpc-url", "https://rpc.example/",
                    "--log-level", "trace",     "--concurrency=16",
                    "--end-slot",  "900"};
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));

    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_config_apply_args(&cfg, argc, argv, &err), IDX_OK);
    TEST_EQ_STR(cfg.rpc_url, "https://rpc.example/");
    TEST_EQ_INT(cfg.log_level, IDX_LOG_TRACE);
    TEST_EQ_UINT(cfg.concurrency, 16u);
    TEST_EQ_UINT(cfg.end_slot, 900u);
}

static void test_args_errors(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);

    char *missing_value[] = {"indexer", "--rpc-url"};
    TEST_EQ_INT(idx_config_apply_args(&cfg, 2, missing_value, NULL),
                IDX_ERR_PARSE);

    char *unknown[] = {"indexer", "--nonsense", "1"};
    TEST_EQ_INT(idx_config_apply_args(&cfg, 3, unknown, NULL), IDX_ERR_NOT_FOUND);

    char *stray[] = {"indexer", "positional"};
    TEST_EQ_INT(idx_config_apply_args(&cfg, 2, stray, NULL), IDX_ERR_PARSE);
}

static void test_help_flag(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);

    char *argv[] = {"indexer", "--help"};
    TEST_EQ_INT(idx_config_apply_args(&cfg, 2, argv, NULL), IDX_OK);
    TEST_ASSERT(cfg.help);
}

static void test_apply_env(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);

    setenv("SOLANA_RPC_URL", "https://env.example/", 1);
    setenv("INDEXER_CONCURRENCY", "8", 1);
    setenv("INDEXER_LOG_LEVEL", "warn", 1);

    TEST_EQ_INT(idx_config_apply_env(&cfg, NULL), IDX_OK);
    TEST_EQ_STR(cfg.rpc_url, "https://env.example/");
    TEST_EQ_UINT(cfg.concurrency, 8u);
    TEST_EQ_INT(cfg.log_level, IDX_LOG_WARN);

    /* An empty variable must not override anything. */
    setenv("SOLANA_RPC_URL", "", 1);
    TEST_EQ_INT(idx_config_apply_env(&cfg, NULL), IDX_OK);
    TEST_EQ_STR(cfg.rpc_url, "https://env.example/");

    unsetenv("SOLANA_RPC_URL");
    unsetenv("INDEXER_CONCURRENCY");
    unsetenv("INDEXER_LOG_LEVEL");
}

static const char *write_temp_config(const char *contents) {
    static char path[] = "/tmp/idx_config_test.conf";
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return NULL;
    }
    fputs(contents, file);
    fclose(file);
    return path;
}

static void test_apply_file(void) {
    const char *path = write_temp_config(
        "# endpoints\n"
        "rpc_url = https://file.example/\n"
        "wss_url=wss://file.example/\n"
        "\n"
        "  concurrency = 12   # inline comment\n"
        "log_level = debug\n");
    TEST_ASSERT(path != NULL);

    idx_config cfg;
    idx_config_defaults(&cfg);

    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_config_apply_file(&cfg, path, &err), IDX_OK);
    TEST_EQ_STR(cfg.rpc_url, "https://file.example/");
    TEST_EQ_STR(cfg.wss_url, "wss://file.example/");
    TEST_EQ_UINT(cfg.concurrency, 12u);
    TEST_EQ_INT(cfg.log_level, IDX_LOG_DEBUG);

    remove(path);
}

static void test_file_errors(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);

    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_config_apply_file(&cfg, "/nonexistent/indexer.conf", &err),
                IDX_ERR_IO);
    TEST_ASSERT(err.message[0] != '\0');

    const char *path = write_temp_config("rpc_url\n");
    TEST_ASSERT(path != NULL);
    idx_error_clear(&err);
    TEST_EQ_INT(idx_config_apply_file(&cfg, path, &err), IDX_ERR_PARSE);
    TEST_ASSERT(strstr(err.message, ":1:") != NULL);
    remove(path);
}

/* The command line must win over the environment, which wins over the file. */
static void test_precedence(void) {
    const char *path = write_temp_config(
        "rpc_url = https://file.example/\n"
        "concurrency = 2\n"
        "start_slot = 100\n");
    TEST_ASSERT(path != NULL);

    setenv("SOLANA_RPC_URL", "https://env.example/", 1);
    setenv("INDEXER_CONCURRENCY", "3", 1);

    char *argv[] = {"indexer", "--config", (char *)path, "--concurrency", "4"};
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));

    idx_config cfg;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_config_load(&cfg, argc, argv, &err), IDX_OK);

    TEST_EQ_UINT(cfg.start_slot, 100u);              /* only in the file */
    TEST_EQ_STR(cfg.rpc_url, "https://env.example/"); /* env over file */
    TEST_EQ_UINT(cfg.concurrency, 4u);                /* CLI over env */

    unsetenv("SOLANA_RPC_URL");
    unsetenv("INDEXER_CONCURRENCY");
    remove(path);
}

static void test_validate(void) {
    idx_config cfg;
    idx_config_defaults(&cfg);

    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_config_validate(&cfg, &err), IDX_ERR_INVALID_ARG);
    TEST_ASSERT(strstr(err.message, "rpc_url") != NULL);

    idx_config_apply_kv(&cfg, "rpc_url", "ftp://rpc.example/", "test", NULL);
    TEST_EQ_INT(idx_config_validate(&cfg, NULL), IDX_ERR_INVALID_ARG);

    idx_config_apply_kv(&cfg, "rpc_url", "https://rpc.example/", "test", NULL);
    TEST_EQ_INT(idx_config_validate(&cfg, NULL), IDX_OK);

    idx_config_apply_kv(&cfg, "wss_url", "https://rpc.example/", "test", NULL);
    TEST_EQ_INT(idx_config_validate(&cfg, NULL), IDX_ERR_INVALID_ARG);
    idx_config_apply_kv(&cfg, "wss_url", "wss://rpc.example/", "test", NULL);
    TEST_EQ_INT(idx_config_validate(&cfg, NULL), IDX_OK);

    idx_config_apply_kv(&cfg, "start_slot", "500", "test", NULL);
    idx_config_apply_kv(&cfg, "end_slot", "400", "test", NULL);
    TEST_EQ_INT(idx_config_validate(&cfg, NULL), IDX_ERR_RANGE);

    idx_config_apply_kv(&cfg, "end_slot", "0", "test", NULL);
    TEST_EQ_INT(idx_config_validate(&cfg, NULL), IDX_OK); /* 0 = follow */
}

TEST_MAIN({
    TEST_RUN(test_defaults);
    TEST_RUN(test_subscription_settings);
    TEST_RUN(test_tx_details_names);
    TEST_RUN(test_block_subscribe_params);
    TEST_RUN(test_apply_kv);
    TEST_RUN(test_string_too_long);
    TEST_RUN(test_apply_args);
    TEST_RUN(test_args_errors);
    TEST_RUN(test_help_flag);
    TEST_RUN(test_apply_env);
    TEST_RUN(test_apply_file);
    TEST_RUN(test_file_errors);
    TEST_RUN(test_precedence);
    TEST_RUN(test_validate);
})
