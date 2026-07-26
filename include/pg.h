/*
 * PostgreSQL client over libpq (ROADMAP.md milestone M7, decision D4).
 *
 * The confirmed tier is PostgreSQL (D4): the mutable, unfinalized window, where
 * a reorg is a delete-and-rewrite in one transaction and bars are recomputed by
 * a SQL aggregate. This module is the libpq wrapper the tier is built on —
 * connection handling and prepared statements — and nothing above the schema.
 * It knows no entity and holds no table; idx_store's confirmed backend (a later
 * M7 item) is what turns write sets into SQL through it.
 *
 * Three things it does that a raw PQexec loop does not:
 *
 *   - It maps libpq's result codes onto idx_status, so a caller sees
 *     IDX_ERR_NETWORK when the connection dropped and IDX_ERR_REMOTE when the
 *     server rejected a statement, and gets the SQLSTATE and server message in
 *     the idx_error rather than having to reach back into the PGresult.
 *   - It remembers every prepared statement, so idx_pg_reset can reconnect and
 *     re-prepare them: prepared statements are per-session, and a reconnect
 *     silently loses them otherwise.
 *   - It wraps BEGIN/COMMIT/ROLLBACK, which is the unit the reorg path needs.
 *
 * libpq itself is reached only from src/pg.c; nothing else includes libpq-fe.h,
 * the same containment the transport modules give libcurl (decision D1). The
 * whole module compiles to nothing when libpq is absent (IDX_HAVE_LIBPQ
 * undefined), so the project still builds without it — storage is optional
 * until a deployment configures a tier.
 *
 * A connection belongs to one thread, like the libpq PGconn it wraps.
 */
#ifndef IDX_PG_H
#define IDX_PG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"

typedef struct idx_pg_conn idx_pg_conn;
typedef struct idx_pg_result idx_pg_result;

/* ----------------------------------------------------------- connection ---- */

/*
 * Opens a connection from a libpq conninfo string (a DSN or a
 * "host=... port=... dbname=..." key/value list; see the libpq documentation).
 * The connection is verified up, so a returned handle is ready to use.
 *
 *   IDX_OK             `*out` owns a connection; close it with idx_pg_close
 *   IDX_ERR_INVALID_ARG  conninfo or out is NULL
 *   IDX_ERR_NO_MEMORY  libpq could not allocate the connection object
 *   IDX_ERR_NETWORK    the server could not be reached or refused the login;
 *                      the server's message is in `err`
 */
idx_status idx_pg_connect(const char *conninfo, idx_pg_conn **out,
                          idx_error *err);

/* Closes and frees the connection. Safe to call with NULL. */
void idx_pg_close(idx_pg_conn *conn);

/* Whether the connection last looked healthy. This is libpq's cached view and
 * does not touch the server; idx_pg_ping does. */
bool idx_pg_is_ok(const idx_pg_conn *conn);

/*
 * A round trip that confirms the server answers ("SELECT 1"). Unlike
 * idx_pg_is_ok it detects a connection that died since the last statement.
 *
 *   IDX_OK           the server answered
 *   IDX_ERR_NETWORK  it did not; the caller may idx_pg_reset and retry
 */
idx_status idx_pg_ping(idx_pg_conn *conn, idx_error *err);

/*
 * Reconnects a dropped (or still-live) connection and re-prepares every
 * statement registered through idx_pg_prepare, since a new session starts with
 * none. On failure the connection is left closed and unusable until a later
 * reset succeeds.
 *
 *   IDX_OK           connected and every statement re-prepared
 *   IDX_ERR_NETWORK  the reconnect failed
 *   IDX_ERR_REMOTE   a statement would not re-prepare on the fresh session
 */
idx_status idx_pg_reset(idx_pg_conn *conn, idx_error *err);

/* The server's version as libpq reports it (e.g. 170004 for 17.4), or 0 when
 * the connection is down. */
int idx_pg_server_version(const idx_pg_conn *conn);

/* ---------------------------------------------------------- statements ----- */

/*
 * Runs a statement with no parameters, for DDL and session control. `out` may
 * be NULL when the result is not read (the usual case for DDL); when it is not,
 * `*out` owns a result to free with idx_pg_result_free.
 *
 *   IDX_OK           executed
 *   IDX_ERR_REMOTE   the server rejected it; the SQLSTATE and message are in
 *                    `err`
 *   IDX_ERR_NETWORK  the connection dropped mid-statement
 */
idx_status idx_pg_exec(idx_pg_conn *conn, const char *sql, idx_pg_result **out,
                       idx_error *err);

/* BEGIN / COMMIT / ROLLBACK, the transaction the reorg path runs in (D4). */
idx_status idx_pg_begin(idx_pg_conn *conn, idx_error *err);
idx_status idx_pg_commit(idx_pg_conn *conn, idx_error *err);
idx_status idx_pg_rollback(idx_pg_conn *conn, idx_error *err);

/*
 * Prepares `sql` under `name`, a caller-chosen identifier unique on the
 * connection, taking `n_params` parameters written `$1`..`$n`. The client keeps
 * the name and text so idx_pg_reset can re-prepare it; preparing the same name
 * twice replaces the remembered entry.
 *
 *   IDX_OK             prepared and remembered
 *   IDX_ERR_REMOTE     the server rejected the statement (a syntax error)
 *   IDX_ERR_NETWORK    the connection dropped
 *   IDX_ERR_NO_MEMORY  the statement table could not grow
 */
idx_status idx_pg_prepare(idx_pg_conn *conn, const char *name, const char *sql,
                          int n_params, idx_error *err);

/*
 * Executes the statement prepared as `name`, binding `n_params` text values in
 * `$1`..`$n` order. A NULL element binds SQL NULL. All parameters are sent in
 * text form, which is what the insert path uses; a binary path can come later
 * if profiling asks for it. `out` may be NULL to discard the result.
 *
 *   IDX_OK             executed
 *   IDX_ERR_INVALID_ARG  n_params disagrees with what was prepared, or values
 *                        is NULL with n_params > 0
 *   IDX_ERR_REMOTE     the server rejected it (a constraint violation carries
 *                      its SQLSTATE in `err`)
 *   IDX_ERR_NETWORK    the connection dropped
 */
idx_status idx_pg_exec_prepared(idx_pg_conn *conn, const char *name,
                                int n_params, const char *const *values,
                                idx_pg_result **out, idx_error *err);

/* ------------------------------------------------------------- results ----- */

void idx_pg_result_free(idx_pg_result *res);

/* Rows and columns the result carries. Zero rows is normal for a command. */
size_t idx_pg_result_rows(const idx_pg_result *res);
size_t idx_pg_result_cols(const idx_pg_result *res);

/* Number of rows a non-SELECT command reported affected (INSERT/UPDATE/DELETE),
 * or 0 when the command reports none. */
uint64_t idx_pg_result_affected(const idx_pg_result *res);

/* Whether `(row, col)` is SQL NULL. Out-of-range indices read as NULL. */
bool idx_pg_result_is_null(const idx_pg_result *res, size_t row, size_t col);

/*
 * The text of `(row, col)`, borrowing the result's storage (valid until it is
 * freed). Returns NULL for a SQL NULL or an out-of-range index — the two are
 * told apart with idx_pg_result_is_null.
 */
const char *idx_pg_result_text(const idx_pg_result *res, size_t row, size_t col);

/*
 * `(row, col)` parsed as a signed/unsigned 64-bit integer.
 *
 *   IDX_OK             `*out` is set
 *   IDX_ERR_NOT_FOUND  the cell is SQL NULL or out of range
 *   IDX_ERR_PARSE      the text is not an integer, or overflows the type
 */
idx_status idx_pg_result_i64(const idx_pg_result *res, size_t row, size_t col,
                             int64_t *out, idx_error *err);
idx_status idx_pg_result_u64(const idx_pg_result *res, size_t row, size_t col,
                             uint64_t *out, idx_error *err);

#endif /* IDX_PG_H */
