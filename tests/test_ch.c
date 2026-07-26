/*
 * ClickHouse HTTP client tests.
 *
 * Offline: argument checks and that an unreachable endpoint fails as a network
 * error. Online (only when IDX_TEST_CH_URL points at a server): ping, a query
 * round trip, an insert with a body in a named format, and that a bad statement
 * comes back as a remote error carrying ClickHouse's exception code.
 */
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "test.h"

static long long scalar(idx_ch_conn *c, const char *sql) {
    idx_ch_result *r = NULL;
    if (idx_ch_query(c, sql, &r, NULL) != IDX_OK) {
        return -1;
    }
    const char *body = idx_ch_result_body(r);
    long long v = body != NULL ? atoll(body) : -1;
    idx_ch_result_free(r);
    return v;
}

static void test_offline(void) {
    idx_ch_options opt;
    idx_ch_options_init(&opt);
    TEST_EQ_INT(opt.timeout_ms, 30000);
    TEST_EQ_INT(opt.connect_timeout_ms, 5000);

    idx_error err;
    idx_error_clear(&err);
    idx_ch_conn *conn = NULL;
    /* Missing url. */
    TEST_EQ_INT(idx_ch_open(&opt, &conn, &err), IDX_ERR_INVALID_ARG);

    /* A well-formed but unreachable endpoint opens (no contact yet) and fails
     * on the first request. */
    opt.url = "http://127.0.0.1:1";
    opt.connect_timeout_ms = 1500;
    opt.timeout_ms = 2000;
    TEST_EQ_INT(idx_ch_open(&opt, &conn, &err), IDX_OK);
    TEST_ASSERT(conn != NULL);
    TEST_EQ_INT(idx_ch_ping(conn, &err), IDX_ERR_NETWORK);
    idx_ch_close(conn);

    idx_ch_close(NULL);
    idx_ch_result_free(NULL);
    TEST_ASSERT(idx_ch_result_body(NULL) == NULL);
    TEST_EQ_INT(idx_ch_exception_code(NULL), 0);
}

static void test_online(void) {
    idx_ch_options opt;
    idx_ch_options_init(&opt);
    opt.url = getenv("IDX_TEST_CH_URL");

    idx_error err;
    idx_error_clear(&err);
    idx_ch_conn *conn = NULL;
    TEST_EQ_INT(idx_ch_open(&opt, &conn, &err), IDX_OK);
    if (conn == NULL) {
        return;
    }

    TEST_EQ_INT(idx_ch_ping(conn, &err), IDX_OK);

    /* A query with a response. */
    idx_ch_result *ver = NULL;
    TEST_EQ_INT(idx_ch_query(conn, "SELECT version()", &ver, &err), IDX_OK);
    TEST_ASSERT(idx_ch_result_len(ver) > 0);
    idx_ch_result_free(ver);

    TEST_EQ_INT(idx_ch_query(conn, "DROP TABLE IF EXISTS idx_ch_selftest", NULL,
                             &err),
                IDX_OK);
    TEST_EQ_INT(idx_ch_query(conn,
                             "CREATE TABLE idx_ch_selftest (id Int64, name "
                             "String) ENGINE = MergeTree ORDER BY id",
                             NULL, &err),
                IDX_OK);

    /* An inline insert. */
    TEST_EQ_INT(idx_ch_query(conn,
                             "INSERT INTO idx_ch_selftest VALUES (1, 'one')",
                             NULL, &err),
                IDX_OK);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_ch_selftest"), 1);

    /* An insert whose rows are the request body, in a named format. */
    const char *rows = "2\ttwo\n3\tthree\n";
    TEST_EQ_INT(idx_ch_insert(conn,
                              "INSERT INTO idx_ch_selftest FORMAT TabSeparated",
                              rows, strlen(rows), &err),
                IDX_OK);
    TEST_EQ_INT(scalar(conn, "SELECT count() FROM idx_ch_selftest"), 3);
    TEST_EQ_INT(scalar(conn, "SELECT sum(id) FROM idx_ch_selftest"), 6);
    /* A successful request clears the exception code. */
    TEST_EQ_INT(idx_ch_exception_code(conn), 0);

    /* A server-side error: a syntax error comes back as remote, with a code. */
    TEST_EQ_INT(idx_ch_query(conn, "SELEXT 1", NULL, &err), IDX_ERR_REMOTE);
    TEST_ASSERT(idx_ch_exception_code(conn) != 0);

    /* An insert into a missing table is remote too. */
    TEST_EQ_INT(idx_ch_insert(conn, "INSERT INTO no_such_table FORMAT "
                                    "TabSeparated",
                              "1\n", 2, &err),
                IDX_ERR_REMOTE);

    TEST_EQ_INT(idx_ch_query(conn, "DROP TABLE IF EXISTS idx_ch_selftest", NULL,
                             &err),
                IDX_OK);
    idx_ch_close(conn);
}

TEST_MAIN({
    TEST_RUN(test_offline);
    if (getenv("IDX_TEST_CH_URL") != NULL) {
        TEST_RUN(test_online);
    } else {
        printf("  (skipping ch online tests: set IDX_TEST_CH_URL)\n");
    }
})
