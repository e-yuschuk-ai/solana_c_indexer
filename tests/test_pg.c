/*
 * PostgreSQL client tests.
 *
 * Split in two. The offline half needs no server: it checks argument
 * validation and that an unreachable endpoint reports IDX_ERR_NETWORK rather
 * than hanging or crashing. The online half runs only when
 * IDX_TEST_PG_CONNINFO points at a database (docker-compose brings one up; see
 * the README) and exercises the real path — prepared statements, parameters,
 * transactions, reconnection and the server's own errors. Without that variable
 * the online half is skipped, so `make test` stays green on a machine with no
 * PostgreSQL.
 */
#include <stdlib.h>

#include "pg.h"
#include "test.h"

static void test_connect_errors(void) {
    idx_error err;
    idx_error_clear(&err);

    idx_pg_conn *conn = NULL;
    TEST_EQ_INT(idx_pg_connect(NULL, &conn, &err), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_pg_connect("", NULL, &err), IDX_ERR_INVALID_ARG);

    /* A refused port fails as a network error, promptly. */
    TEST_EQ_INT(idx_pg_connect("host=127.0.0.1 port=1 dbname=x user=x "
                               "connect_timeout=2",
                               &conn, &err),
                IDX_ERR_NETWORK);

    /* NULL-tolerant accessors and frees. */
    TEST_ASSERT(!idx_pg_is_ok(NULL));
    TEST_EQ_INT(idx_pg_server_version(NULL), 0);
    idx_pg_close(NULL);
    idx_pg_result_free(NULL);
    TEST_ASSERT(idx_pg_result_text(NULL, 0, 0) == NULL);
    TEST_ASSERT(idx_pg_result_is_null(NULL, 0, 0));
    TEST_EQ_UINT(idx_pg_result_rows(NULL), 0);
}

static void test_pg_online(void) {
    const char *conninfo = getenv("IDX_TEST_PG_CONNINFO");
    idx_error err;
    idx_error_clear(&err);

    idx_pg_conn *conn = NULL;
    TEST_EQ_INT(idx_pg_connect(conninfo, &conn, &err), IDX_OK);
    if (conn == NULL) {
        return;
    }
    TEST_ASSERT(idx_pg_is_ok(conn));
    TEST_ASSERT(idx_pg_server_version(conn) > 0);
    TEST_EQ_INT(idx_pg_ping(conn, &err), IDX_OK);

    /* A permanent table, not a temp one: idx_pg_reset reconnects onto a fresh
     * session, and the "ins" statement re-prepared there must still find its
     * table (a temp table would be gone). Dropped at the end. */
    TEST_EQ_INT(idx_pg_exec(conn, "DROP TABLE IF EXISTS idx_pg_selftest", NULL,
                            &err),
                IDX_OK);
    TEST_EQ_INT(idx_pg_exec(conn,
                            "CREATE TABLE idx_pg_selftest "
                            "(id bigint PRIMARY KEY, name text)",
                            NULL, &err),
                IDX_OK);

    TEST_EQ_INT(idx_pg_prepare(conn, "ins",
                               "INSERT INTO idx_pg_selftest(id, name) "
                               "VALUES($1::bigint, $2::text)",
                               2, &err),
                IDX_OK);

    const char *row1[] = {"1", "hello"};
    idx_pg_result *ins = NULL;
    TEST_EQ_INT(idx_pg_exec_prepared(conn, "ins", 2, row1, &ins, &err), IDX_OK);
    TEST_EQ_UINT(idx_pg_result_affected(ins), 1);
    idx_pg_result_free(ins);

    /* A NULL element binds SQL NULL. */
    const char *row2[] = {"2", NULL};
    TEST_EQ_INT(idx_pg_exec_prepared(conn, "ins", 2, row2, NULL, &err), IDX_OK);

    /* Read them back. */
    idx_pg_result *sel = NULL;
    TEST_EQ_INT(idx_pg_exec(conn, "SELECT id, name FROM idx_pg_selftest ORDER BY id", &sel,
                            &err),
                IDX_OK);
    TEST_EQ_UINT(idx_pg_result_rows(sel), 2);
    TEST_EQ_UINT(idx_pg_result_cols(sel), 2);
    uint64_t id = 0;
    TEST_EQ_INT(idx_pg_result_u64(sel, 0, 0, &id, &err), IDX_OK);
    TEST_EQ_UINT(id, 1);
    TEST_EQ_STR(idx_pg_result_text(sel, 0, 1), "hello");
    TEST_ASSERT(!idx_pg_result_is_null(sel, 0, 1));
    /* Row 2's name is NULL. */
    TEST_ASSERT(idx_pg_result_is_null(sel, 1, 1));
    TEST_ASSERT(idx_pg_result_text(sel, 1, 1) == NULL);
    idx_pg_result_free(sel);

    /* A signed read and an out-of-range cell. */
    idx_pg_result *one = NULL;
    TEST_EQ_INT(idx_pg_exec(conn, "SELECT -7", &one, &err), IDX_OK);
    int64_t neg = 0;
    TEST_EQ_INT(idx_pg_result_i64(one, 0, 0, &neg, &err), IDX_OK);
    TEST_EQ_INT(neg, -7);
    /* A negative never reads back as a huge unsigned. */
    TEST_EQ_INT(idx_pg_result_u64(one, 0, 0, NULL, &err), IDX_ERR_PARSE);
    TEST_EQ_INT(idx_pg_result_i64(one, 5, 0, NULL, &err), IDX_ERR_NOT_FOUND);
    idx_pg_result_free(one);

    /* -------- transactions -------- */
    /* A rolled-back insert leaves nothing behind. */
    TEST_EQ_INT(idx_pg_begin(conn, &err), IDX_OK);
    const char *row3[] = {"3", "rolled-back"};
    TEST_EQ_INT(idx_pg_exec_prepared(conn, "ins", 2, row3, NULL, &err), IDX_OK);
    TEST_EQ_INT(idx_pg_rollback(conn, &err), IDX_OK);
    /* A committed one stays. */
    TEST_EQ_INT(idx_pg_begin(conn, &err), IDX_OK);
    const char *row4[] = {"4", "committed"};
    TEST_EQ_INT(idx_pg_exec_prepared(conn, "ins", 2, row4, NULL, &err), IDX_OK);
    TEST_EQ_INT(idx_pg_commit(conn, &err), IDX_OK);

    idx_pg_result *cnt = NULL;
    TEST_EQ_INT(idx_pg_exec(conn, "SELECT count(*) FROM idx_pg_selftest", &cnt, &err), IDX_OK);
    uint64_t n = 0;
    TEST_EQ_INT(idx_pg_result_u64(cnt, 0, 0, &n, &err), IDX_OK);
    TEST_EQ_UINT(n, 3); /* ids 1, 2, 4 */
    idx_pg_result_free(cnt);

    /* -------- server-side errors -------- */
    /* A syntax error is the server rejecting a statement, not a dropped link. */
    TEST_EQ_INT(idx_pg_prepare(conn, "bad", "SELEXT 1", 0, &err), IDX_ERR_REMOTE);
    /* A constraint violation is remote too, and carries its SQLSTATE. */
    const char *dup[] = {"1", "dup"};
    TEST_EQ_INT(idx_pg_exec_prepared(conn, "ins", 2, dup, NULL, &err),
                IDX_ERR_REMOTE);
    /* Wrong parameter count and unknown statement are caught before the wire. */
    TEST_EQ_INT(idx_pg_exec_prepared(conn, "ins", 1, dup, NULL, &err),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_pg_exec_prepared(conn, "nope", 0, NULL, NULL, &err),
                IDX_ERR_INVALID_ARG);

    /* -------- reconnect re-prepares -------- */
    /* Both remembered statements ("ins" and this one) are re-prepared onto the
     * fresh session the reset opens; "ins" finds its permanent table there. */
    TEST_EQ_INT(idx_pg_prepare(conn, "echo", "SELECT $1::bigint", 1, &err),
                IDX_OK);
    TEST_EQ_INT(idx_pg_reset(conn, &err), IDX_OK);
    TEST_ASSERT(idx_pg_is_ok(conn));
    const char *echo[] = {"99"};
    idx_pg_result *er = NULL;
    TEST_EQ_INT(idx_pg_exec_prepared(conn, "echo", 1, echo, &er, &err), IDX_OK);
    uint64_t back = 0;
    TEST_EQ_INT(idx_pg_result_u64(er, 0, 0, &back, &err), IDX_OK);
    TEST_EQ_UINT(back, 99);
    idx_pg_result_free(er);

    TEST_EQ_INT(idx_pg_exec(conn, "DROP TABLE IF EXISTS idx_pg_selftest", NULL,
                            &err),
                IDX_OK);
    idx_pg_close(conn);
}

TEST_MAIN({
    TEST_RUN(test_connect_errors);
    if (getenv("IDX_TEST_PG_CONNINFO") != NULL) {
        TEST_RUN(test_pg_online);
    } else {
        printf("  (skipping pg online tests: set IDX_TEST_PG_CONNINFO)\n");
    }
})
