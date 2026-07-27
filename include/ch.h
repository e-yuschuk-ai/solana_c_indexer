/*
 * ClickHouse HTTP client (ROADMAP.md milestone M7, decision D4).
 *
 * The finalized tier is ClickHouse (D4): append-only, the durable side a reorg
 * can never reach. ClickHouse speaks two protocols; this uses the HTTP one
 * (port 8123 by convention), which needs only libcurl — already a dependency
 * (D1) — rather than the native TCP protocol and its own client library.
 *
 * This is the transport, not the schema: it sends a query or an insert and
 * hands back the response bytes or an error. Two things it does that a bare
 * libcurl POST does not:
 *
 *   - it separates the query from the data, so an insert can carry a binary
 *     body (RowBinary, built by idx_ch_rows) with the statement in the URL;
 *   - it maps ClickHouse's failures onto idx_status — a transport failure is
 *     IDX_ERR_NETWORK, and a server exception is IDX_ERR_REMOTE carrying the
 *     numeric exception code ClickHouse reports in the
 *     X-ClickHouse-Exception-Code header, which is how a caller tells a
 *     retryable overload (TOO_MANY_PARTS, MEMORY_LIMIT_EXCEEDED) from a fatal
 *     one.
 *
 * libcurl is reached only from src/ch.c. A connection wraps one reusable easy
 * handle and belongs to one thread, like the RPC client.
 */
#ifndef IDX_CH_H
#define IDX_CH_H

#include <stddef.h>
#include <stdint.h>

#include "error.h"

typedef struct idx_ch_conn idx_ch_conn;
typedef struct idx_ch_result idx_ch_result;

typedef struct {
    /* Base endpoint, e.g. "http://127.0.0.1:8123". Required. */
    const char *url;
    /* Default database for statements that do not qualify their tables. NULL
     * or empty selects the server default. */
    const char *database;
    /* Credentials. NULL user selects "default"; NULL password is empty. */
    const char *user;
    const char *password;
    /* 0 selects a built-in default (see idx_ch_options_init). */
    long timeout_ms;
    long connect_timeout_ms;
} idx_ch_options;

/* Fills `options` with defaults: no database override, user "default", empty
 * password, 30 s request timeout, 5 s connect timeout. `url` is left NULL and
 * must be set by the caller. */
void idx_ch_options_init(idx_ch_options *options);

/*
 * Opens a connection. Nothing is sent here — the endpoint is contacted on the
 * first query — so this fails only on a bad URL or an allocation error.
 *
 *   IDX_OK             `*out` owns a connection; close it with idx_ch_close
 *   IDX_ERR_INVALID_ARG  options, its url, or out is NULL
 *   IDX_ERR_NO_MEMORY  allocation failed
 */
idx_status idx_ch_open(const idx_ch_options *options, idx_ch_conn **out,
                       idx_error *err);

void idx_ch_close(idx_ch_conn *conn);

/*
 * A liveness check: GET /ping, which a healthy server answers "Ok.".
 *
 *   IDX_OK           the server answered
 *   IDX_ERR_NETWORK  it did not
 */
idx_status idx_ch_ping(idx_ch_conn *conn, idx_error *err);

/*
 * Runs `sql` (the statement in the request body) and captures the response.
 * Used for DDL, for a SELECT (append `FORMAT ...` and parse the body), and for
 * an INSERT whose values are inline. `out` may be NULL to discard the body.
 *
 *   IDX_OK           executed; `*out` owns the response when requested
 *   IDX_ERR_NETWORK  the request did not complete
 *   IDX_ERR_TIMEOUT  it exceeded the configured timeout
 *   IDX_ERR_REMOTE   the server raised an exception; the code and message are
 *                    in `err`, and idx_ch_result_exception_code has the number
 */
idx_status idx_ch_query(idx_ch_conn *conn, const char *sql,
                        idx_ch_result **out, idx_error *err);

/*
 * Runs an insert whose body is `data` — `len` bytes in the format `sql` names,
 * e.g. "INSERT INTO t FORMAT RowBinary". The statement travels in the URL so
 * the body is exactly the data, which is what lets the body be binary. `data`
 * may be NULL only when `len` is 0.
 *
 * Same status mapping as idx_ch_query; the response is discarded (an insert
 * returns none).
 */
idx_status idx_ch_insert(idx_ch_conn *conn, const char *sql, const void *data,
                         size_t len, idx_error *err);

/* ------------------------------------------------------------- results ----- */

void idx_ch_result_free(idx_ch_result *res);

/* The response body and its length. The body is NUL-terminated for text
 * convenience, but `len` is authoritative (a binary body may contain NULs).
 * Returns NULL / 0 for a NULL result. */
const char *idx_ch_result_body(const idx_ch_result *res);
size_t idx_ch_result_len(const idx_ch_result *res);

/*
 * The ClickHouse exception code from the last failed request on `conn`, or 0
 * when the last request succeeded. Exposed so a caller can special-case the
 * retryable ones (e.g. 252 TOO_MANY_PARTS) without parsing the message.
 */
int idx_ch_exception_code(const idx_ch_conn *conn);

#endif /* IDX_CH_H */
