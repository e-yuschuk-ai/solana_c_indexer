/*
 * Minimal assertion harness.
 *
 * Each tests/test_*.c file is a standalone program: it defines TEST_MAIN with
 * a body of TEST_RUN(...) calls and exits non-zero if anything failed.
 */
#ifndef IDX_TEST_H
#define IDX_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int idx_test_failures = 0;
static int idx_test_checks = 0;
static const char *idx_test_current = "";

#define TEST_CHECK(cond, ...)                                             \
    do {                                                                  \
        idx_test_checks++;                                                \
        if (!(cond)) {                                                    \
            idx_test_failures++;                                          \
            fprintf(stderr, "  FAIL %s (%s:%d): ", idx_test_current,      \
                    __FILE__, __LINE__);                                  \
            fprintf(stderr, __VA_ARGS__);                                 \
            fprintf(stderr, "\n");                                        \
        }                                                                 \
    } while (0)

#define TEST_ASSERT(cond) TEST_CHECK((cond), "expected %s", #cond)

#define TEST_EQ_INT(actual, expected)                                     \
    TEST_CHECK((long long)(actual) == (long long)(expected),              \
               "%s: expected %lld, got %lld", #actual,                    \
               (long long)(expected), (long long)(actual))

#define TEST_EQ_UINT(actual, expected)                                    \
    TEST_CHECK((unsigned long long)(actual) ==                            \
                   (unsigned long long)(expected),                        \
               "%s: expected %llu, got %llu", #actual,                    \
               (unsigned long long)(expected),                            \
               (unsigned long long)(actual))

#define TEST_EQ_STR(actual, expected)                                     \
    TEST_CHECK((actual) != NULL && strcmp((actual), (expected)) == 0,     \
               "%s: expected \"%s\", got \"%s\"", #actual, (expected),    \
               ((actual) != NULL) ? (actual) : "(null)")

#define TEST_RUN(fn)              \
    do {                          \
        idx_test_current = #fn;   \
        fn();                     \
    } while (0)

#define TEST_MAIN(body)                                                   \
    int main(void) {                                                      \
        body;                                                             \
        printf("  %d checks, %d failed\n", idx_test_checks,               \
               idx_test_failures);                                        \
        return (idx_test_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;    \
    }

#endif /* IDX_TEST_H */
