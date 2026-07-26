/*
 * PostgreSQL client over libpq. See include/pg.h for the contract.
 *
 * libpq is reached only from here. The module compiles to nothing unless
 * IDX_HAVE_LIBPQ is defined (the Makefile defines it when it finds libpq), so a
 * build without PostgreSQL still links.
 */
#include "pg.h"

#ifdef IDX_HAVE_LIBPQ

#include <errno.h>
#include <libpq-fe.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "vec.h"

/* A statement remembered so idx_pg_reset can re-prepare it on a new session. */
typedef struct {
    char *name;
    char *sql;
    int n_params;
} prepared_stmt;

struct idx_pg_conn {
    PGconn *conn;
    idx_vec prepared; /* of prepared_stmt */
};

struct idx_pg_result {
    PGresult *res;
};

/* Copies the first line of a libpq message, which is otherwise multi-line and
 * newline-terminated, into a bounded buffer. */
static void first_line(const char *s, char *out, size_t n) {
    if (n == 0) {
        return;
    }
    size_t i = 0;
    while (s != NULL && s[i] != '\0' && s[i] != '\n' && i + 1 < n) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
}

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

/* A connection is bad (as opposed to the server rejecting a valid statement)
 * when libpq has torn it down; that is the network failure a caller resets. */
static idx_status classify(const idx_pg_conn *conn) {
    return PQstatus(conn->conn) == CONNECTION_BAD ? IDX_ERR_NETWORK
                                                  : IDX_ERR_REMOTE;
}

/* Records a failed result on `err` with its SQLSTATE and message, clears it,
 * and returns the mapped status. `res` may be NULL (a NULL from libpq itself). */
static idx_status fail_result(idx_pg_conn *conn, PGresult *res, idx_error *err,
                              const char *what) {
    idx_status status = classify(conn);
    /* sqlstate and the message point into `res`, so copy both out before the
     * PQclear that frees it. */
    char state[16] = {0};
    char msg[256];
    if (res != NULL) {
        const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        if (sqlstate != NULL) {
            first_line(sqlstate, state, sizeof state);
        }
        first_line(PQresultErrorMessage(res), msg, sizeof msg);
    } else {
        first_line(PQerrorMessage(conn->conn), msg, sizeof msg);
    }
    PQclear(res);
    if (state[0] != '\0') {
        return IDX_FAIL(err, status, "%s: [%s] %s", what, state, msg);
    }
    return IDX_FAIL(err, status, "%s: %s", what, msg);
}

/* Wraps a successful PGresult, or frees it and reports on allocation failure. */
static idx_status wrap_result(PGresult *res, idx_pg_result **out,
                              idx_error *err) {
    if (out == NULL) {
        PQclear(res);
        return IDX_OK;
    }
    idx_pg_result *r = malloc(sizeof *r);
    if (r == NULL) {
        PQclear(res);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "pg result: allocation failed");
    }
    r->res = res;
    *out = r;
    return IDX_OK;
}

/* Turns a just-returned PGresult into an idx_status, wrapping it into `out` when
 * it is a success and the caller wants it. Consumes `res`. */
static idx_status finish(idx_pg_conn *conn, PGresult *res, idx_pg_result **out,
                         idx_error *err, const char *what) {
    if (res == NULL) {
        /* libpq returns NULL only on a dropped connection or OOM. */
        idx_status status = classify(conn);
        char msg[256];
        first_line(PQerrorMessage(conn->conn), msg, sizeof msg);
        return IDX_FAIL(err, status, "%s: %s", what, msg);
    }
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        return fail_result(conn, res, err, what);
    }
    return wrap_result(res, out, err);
}

/* ----------------------------------------------------------- connection ---- */

idx_status idx_pg_connect(const char *conninfo, idx_pg_conn **out,
                          idx_error *err) {
    if (conninfo == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "pg connect: null argument");
    }
    PGconn *pg = PQconnectdb(conninfo);
    if (pg == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "pg connect: allocation failed");
    }
    if (PQstatus(pg) != CONNECTION_OK) {
        char msg[256];
        first_line(PQerrorMessage(pg), msg, sizeof msg);
        PQfinish(pg);
        return IDX_FAIL(err, IDX_ERR_NETWORK, "pg connect: %s", msg);
    }
    idx_pg_conn *conn = calloc(1, sizeof *conn);
    if (conn == NULL) {
        PQfinish(pg);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "pg connect: allocation failed");
    }
    conn->conn = pg;
    idx_vec_init(&conn->prepared, sizeof(prepared_stmt));
    *out = conn;
    return IDX_OK;
}

void idx_pg_close(idx_pg_conn *conn) {
    if (conn == NULL) {
        return;
    }
    for (size_t i = 0; i < idx_vec_len(&conn->prepared); i++) {
        prepared_stmt *p = idx_vec_at(&conn->prepared, i);
        free(p->name);
        free(p->sql);
    }
    idx_vec_free(&conn->prepared);
    PQfinish(conn->conn);
    free(conn);
}

bool idx_pg_is_ok(const idx_pg_conn *conn) {
    return conn != NULL && PQstatus(conn->conn) == CONNECTION_OK;
}

int idx_pg_server_version(const idx_pg_conn *conn) {
    return conn != NULL ? PQserverVersion(conn->conn) : 0;
}

idx_status idx_pg_ping(idx_pg_conn *conn, idx_error *err) {
    if (conn == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "pg ping: null connection");
    }
    PGresult *res = PQexec(conn->conn, "SELECT 1");
    if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK) {
        char msg[256];
        first_line(PQerrorMessage(conn->conn), msg, sizeof msg);
        PQclear(res);
        return IDX_FAIL(err, IDX_ERR_NETWORK, "pg ping: %s", msg);
    }
    PQclear(res);
    return IDX_OK;
}

idx_status idx_pg_reset(idx_pg_conn *conn, idx_error *err) {
    if (conn == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "pg reset: null connection");
    }
    PQreset(conn->conn);
    if (PQstatus(conn->conn) != CONNECTION_OK) {
        char msg[256];
        first_line(PQerrorMessage(conn->conn), msg, sizeof msg);
        return IDX_FAIL(err, IDX_ERR_NETWORK, "pg reset: %s", msg);
    }
    for (size_t i = 0; i < idx_vec_len(&conn->prepared); i++) {
        const prepared_stmt *p = idx_vec_at(&conn->prepared, i);
        PGresult *res = PQprepare(conn->conn, p->name, p->sql, p->n_params, NULL);
        if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK) {
            return fail_result(conn, res, err, "pg reset: re-prepare");
        }
        PQclear(res);
    }
    return IDX_OK;
}

/* ---------------------------------------------------------- statements ----- */

idx_status idx_pg_exec(idx_pg_conn *conn, const char *sql, idx_pg_result **out,
                       idx_error *err) {
    if (conn == NULL || sql == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "pg exec: null argument");
    }
    return finish(conn, PQexec(conn->conn, sql), out, err, "pg exec");
}

idx_status idx_pg_begin(idx_pg_conn *conn, idx_error *err) {
    return idx_pg_exec(conn, "BEGIN", NULL, err);
}

idx_status idx_pg_commit(idx_pg_conn *conn, idx_error *err) {
    return idx_pg_exec(conn, "COMMIT", NULL, err);
}

idx_status idx_pg_rollback(idx_pg_conn *conn, idx_error *err) {
    return idx_pg_exec(conn, "ROLLBACK", NULL, err);
}

/* Replaces or appends the remembered entry for `name`. */
static idx_status remember(idx_pg_conn *conn, const char *name, const char *sql,
                           int n_params, idx_error *err) {
    char *name_copy = dup_str(name);
    char *sql_copy = dup_str(sql);
    if (name_copy == NULL || sql_copy == NULL) {
        free(name_copy);
        free(sql_copy);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "pg prepare: allocation failed");
    }
    for (size_t i = 0; i < idx_vec_len(&conn->prepared); i++) {
        prepared_stmt *p = idx_vec_at(&conn->prepared, i);
        if (strcmp(p->name, name) == 0) {
            free(p->name);
            free(p->sql);
            p->name = name_copy;
            p->sql = sql_copy;
            p->n_params = n_params;
            return IDX_OK;
        }
    }
    prepared_stmt entry = {name_copy, sql_copy, n_params};
    if (idx_vec_push(&conn->prepared, &entry, err) != IDX_OK) {
        free(name_copy);
        free(sql_copy);
        return IDX_ERR_NO_MEMORY;
    }
    return IDX_OK;
}

idx_status idx_pg_prepare(idx_pg_conn *conn, const char *name, const char *sql,
                          int n_params, idx_error *err) {
    if (conn == NULL || name == NULL || sql == NULL || n_params < 0) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "pg prepare: bad argument");
    }
    PGresult *res = PQprepare(conn->conn, name, sql, n_params, NULL);
    if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK) {
        return fail_result(conn, res, err, "pg prepare");
    }
    PQclear(res);
    return remember(conn, name, sql, n_params, err);
}

/* The remembered entry for `name`, or NULL. */
static const prepared_stmt *find_prepared(const idx_pg_conn *conn,
                                          const char *name) {
    for (size_t i = 0; i < idx_vec_len(&conn->prepared); i++) {
        const prepared_stmt *p = idx_vec_at(&conn->prepared, i);
        if (strcmp(p->name, name) == 0) {
            return p;
        }
    }
    return NULL;
}

idx_status idx_pg_exec_prepared(idx_pg_conn *conn, const char *name,
                                int n_params, const char *const *values,
                                idx_pg_result **out, idx_error *err) {
    if (conn == NULL || name == NULL || n_params < 0 ||
        (n_params > 0 && values == NULL)) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "pg exec prepared: bad argument");
    }
    const prepared_stmt *p = find_prepared(conn, name);
    if (p == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "pg exec prepared: unknown statement '%s'", name);
    }
    if (p->n_params != n_params) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "pg exec prepared: '%s' takes %d params, got %d", name,
                        p->n_params, n_params);
    }
    /* Text parameters: lengths and formats are ignored for text, result in
     * text form (0). */
    PGresult *res = PQexecPrepared(conn->conn, name, n_params, values, NULL,
                                   NULL, 0);
    return finish(conn, res, out, err, "pg exec prepared");
}

/* ------------------------------------------------------------- results ----- */

void idx_pg_result_free(idx_pg_result *res) {
    if (res != NULL) {
        PQclear(res->res);
        free(res);
    }
}

size_t idx_pg_result_rows(const idx_pg_result *res) {
    if (res == NULL) {
        return 0;
    }
    int n = PQntuples(res->res);
    return n > 0 ? (size_t)n : 0;
}

size_t idx_pg_result_cols(const idx_pg_result *res) {
    if (res == NULL) {
        return 0;
    }
    int n = PQnfields(res->res);
    return n > 0 ? (size_t)n : 0;
}

uint64_t idx_pg_result_affected(const idx_pg_result *res) {
    if (res == NULL) {
        return 0;
    }
    const char *tuples = PQcmdTuples(res->res);
    if (tuples == NULL || tuples[0] == '\0') {
        return 0;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(tuples, &end, 10);
    if (errno != 0 || end == tuples || *end != '\0') {
        return 0;
    }
    return (uint64_t)v;
}

/* Bounds-checks `(row, col)` and hands back the int indices libpq wants. */
static bool cell_in_range(const idx_pg_result *res, size_t row, size_t col,
                          int *r, int *c) {
    if (res == NULL) {
        return false;
    }
    if (row >= idx_pg_result_rows(res) || col >= idx_pg_result_cols(res)) {
        return false;
    }
    *r = (int)row;
    *c = (int)col;
    return true;
}

bool idx_pg_result_is_null(const idx_pg_result *res, size_t row, size_t col) {
    int r, c;
    if (!cell_in_range(res, row, col, &r, &c)) {
        return true;
    }
    return PQgetisnull(res->res, r, c) != 0;
}

const char *idx_pg_result_text(const idx_pg_result *res, size_t row,
                               size_t col) {
    int r, c;
    if (!cell_in_range(res, row, col, &r, &c)) {
        return NULL;
    }
    if (PQgetisnull(res->res, r, c) != 0) {
        return NULL;
    }
    return PQgetvalue(res->res, r, c);
}

idx_status idx_pg_result_i64(const idx_pg_result *res, size_t row, size_t col,
                             int64_t *out, idx_error *err) {
    const char *text = idx_pg_result_text(res, row, col);
    if (text == NULL) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "pg result: cell is null");
    }
    errno = 0;
    char *end = NULL;
    long long v = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return IDX_FAIL(err, IDX_ERR_PARSE, "pg result: not an int64: %s", text);
    }
    if (out != NULL) {
        *out = (int64_t)v;
    }
    return IDX_OK;
}

idx_status idx_pg_result_u64(const idx_pg_result *res, size_t row, size_t col,
                             uint64_t *out, idx_error *err) {
    const char *text = idx_pg_result_text(res, row, col);
    if (text == NULL) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "pg result: cell is null");
    }
    /* strtoull silently wraps a leading '-'; reject it so a negative never
     * reads back as a huge unsigned. */
    const char *scan = text;
    while (*scan == ' ' || *scan == '\t') {
        scan++;
    }
    if (*scan == '-') {
        return IDX_FAIL(err, IDX_ERR_PARSE, "pg result: not a uint64: %s", text);
    }
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return IDX_FAIL(err, IDX_ERR_PARSE, "pg result: not a uint64: %s", text);
    }
    if (out != NULL) {
        *out = (uint64_t)v;
    }
    return IDX_OK;
}

#else /* !IDX_HAVE_LIBPQ */

/* Avoid an empty translation unit, which ISO C forbids, when libpq is absent. */
typedef int idx_pg_translation_unit_not_empty;

#endif /* IDX_HAVE_LIBPQ */
