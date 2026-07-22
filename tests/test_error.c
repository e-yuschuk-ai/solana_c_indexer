#include "error.h"

#include <string.h>

#include "test.h"

static void test_status_strings(void) {
    TEST_EQ_STR(idx_status_str(IDX_OK), "ok");
    TEST_EQ_STR(idx_status_str(IDX_ERR_NO_MEMORY), "out of memory");
    TEST_EQ_STR(idx_status_str((idx_status)999), "unknown error");
}

static void test_clear(void) {
    idx_error err;
    IDX_FAIL(&err, IDX_ERR_IO, "boom");
    idx_error_clear(&err);

    TEST_EQ_INT(err.status, IDX_OK);
    TEST_ASSERT(err.file == NULL);
    TEST_EQ_INT(err.line, 0);
    TEST_EQ_STR(err.message, "");

    idx_error_clear(NULL); /* must not crash */
}

static void test_fail_records_context(void) {
    idx_error err;
    idx_error_clear(&err);

    idx_status status = IDX_FAIL(&err, IDX_ERR_PARSE, "bad slot: %s", "abc");
    int expected_line = __LINE__ - 1;

    TEST_EQ_INT(status, IDX_ERR_PARSE);
    TEST_EQ_INT(err.status, IDX_ERR_PARSE);
    TEST_EQ_STR(err.message, "bad slot: abc");
    TEST_EQ_INT(err.line, expected_line);
    TEST_ASSERT(err.file != NULL && strstr(err.file, "test_error.c") != NULL);
}

/* A NULL error still returns the status so callers can propagate it. */
static void test_null_error_is_allowed(void) {
    TEST_EQ_INT(IDX_FAIL(NULL, IDX_ERR_RANGE, "ignored %d", 7), IDX_ERR_RANGE);
}

/* Reporting IDX_OK as a failure is a bug; it is surfaced, not silently kept. */
static void test_ok_is_promoted_to_internal(void) {
    idx_error err;
    idx_error_clear(&err);

    TEST_EQ_INT(IDX_FAIL(&err, IDX_OK, "not really fine"), IDX_ERR_INTERNAL);
    TEST_EQ_INT(err.status, IDX_ERR_INTERNAL);
}

static void test_long_message_is_truncated(void) {
    idx_error err;
    idx_error_clear(&err);

    char big[IDX_ERROR_MSG_MAX * 2];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    IDX_FAIL(&err, IDX_ERR_INTERNAL, "%s", big);
    TEST_EQ_UINT(strlen(err.message), IDX_ERROR_MSG_MAX - 1);
}

static idx_status inner(int fail) {
    if (fail) {
        return IDX_ERR_NOT_FOUND;
    }
    return IDX_OK;
}

static idx_status outer(int fail, int *reached_end) {
    IDX_TRY(inner(fail));
    *reached_end = 1;
    return IDX_OK;
}

static void test_try_propagates(void) {
    int reached_end = 0;
    TEST_EQ_INT(outer(1, &reached_end), IDX_ERR_NOT_FOUND);
    TEST_EQ_INT(reached_end, 0);

    TEST_EQ_INT(outer(0, &reached_end), IDX_OK);
    TEST_EQ_INT(reached_end, 1);
}

TEST_MAIN({
    TEST_RUN(test_status_strings);
    TEST_RUN(test_clear);
    TEST_RUN(test_fail_records_context);
    TEST_RUN(test_null_error_is_allowed);
    TEST_RUN(test_ok_is_promoted_to_internal);
    TEST_RUN(test_long_message_is_truncated);
    TEST_RUN(test_try_propagates);
})
