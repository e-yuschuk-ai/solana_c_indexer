#include "log.h"

#include <string.h>

#include "error.h"
#include "test.h"

static void test_level_names(void) {
    TEST_EQ_STR(idx_log_level_name(IDX_LOG_ERROR), "error");
    TEST_EQ_STR(idx_log_level_name(IDX_LOG_WARN), "warn");
    TEST_EQ_STR(idx_log_level_name(IDX_LOG_INFO), "info");
    TEST_EQ_STR(idx_log_level_name(IDX_LOG_DEBUG), "debug");
    TEST_EQ_STR(idx_log_level_name(IDX_LOG_TRACE), "trace");
    TEST_EQ_STR(idx_log_level_name((idx_log_level)42), "unknown");
}

static void test_level_parse(void) {
    idx_log_level level;

    TEST_EQ_INT(idx_log_level_parse("info", &level), IDX_OK);
    TEST_EQ_INT(level, IDX_LOG_INFO);

    TEST_EQ_INT(idx_log_level_parse("TRACE", &level), IDX_OK);
    TEST_EQ_INT(level, IDX_LOG_TRACE);

    TEST_EQ_INT(idx_log_level_parse("warning", &level), IDX_OK);
    TEST_EQ_INT(level, IDX_LOG_WARN);

    TEST_EQ_INT(idx_log_level_parse("verbose", &level), IDX_ERR_PARSE);
    TEST_EQ_INT(idx_log_level_parse(NULL, &level), IDX_ERR_INVALID_ARG);
}

static void test_threshold_filters_output(void) {
    FILE *sink = tmpfile();
    TEST_ASSERT(sink != NULL);
    if (sink == NULL) {
        return;
    }

    idx_log_init(sink, IDX_LOG_WARN);
    TEST_EQ_INT(idx_log_get_level(), IDX_LOG_WARN);

    IDX_ERROR("visible error %d", 1);
    IDX_WARN("visible warning");
    IDX_INFO("suppressed info");
    IDX_DEBUG("suppressed debug");

    rewind(sink);
    char buffer[4096];
    size_t read = fread(buffer, 1, sizeof(buffer) - 1, sink);
    buffer[read] = '\0';

    TEST_ASSERT(strstr(buffer, "visible error 1") != NULL);
    TEST_ASSERT(strstr(buffer, "visible warning") != NULL);
    TEST_ASSERT(strstr(buffer, "suppressed info") == NULL);
    TEST_ASSERT(strstr(buffer, "suppressed debug") == NULL);

    /* Lines carry the level and the originating file. */
    TEST_ASSERT(strstr(buffer, "error") != NULL);
    TEST_ASSERT(strstr(buffer, "test_log.c") != NULL);

    fclose(sink);
    idx_log_init(stderr, IDX_LOG_INFO);
}

/* Arguments of a disabled level must not be evaluated. */
static int g_side_effect = 0;

static int bump(void) {
    g_side_effect++;
    return g_side_effect;
}

static void test_disabled_level_skips_arguments(void) {
    FILE *sink = tmpfile();
    TEST_ASSERT(sink != NULL);
    if (sink == NULL) {
        return;
    }

    idx_log_init(sink, IDX_LOG_ERROR);
    IDX_TRACE("value %d", bump());
    TEST_EQ_INT(g_side_effect, 0);

    IDX_ERROR("value %d", bump());
    TEST_EQ_INT(g_side_effect, 1);

    fclose(sink);
    idx_log_init(stderr, IDX_LOG_INFO);
}

TEST_MAIN({
    TEST_RUN(test_level_names);
    TEST_RUN(test_level_parse);
    TEST_RUN(test_threshold_filters_output);
    TEST_RUN(test_disabled_level_skips_arguments);
})
