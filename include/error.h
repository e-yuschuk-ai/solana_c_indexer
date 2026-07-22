/*
 * Error handling convention for the indexer.
 *
 * Every fallible function returns an `idx_status` and, when it can add context,
 * takes an `idx_error *err` as its last parameter. `err` may always be NULL:
 * callers that only care whether an operation succeeded pass NULL and ignore
 * the detail. Output parameters are only valid when IDX_OK is returned.
 */
#ifndef IDX_ERROR_H
#define IDX_ERROR_H

#include <stddef.h>

typedef enum {
    IDX_OK = 0,
    IDX_ERR_INVALID_ARG,
    IDX_ERR_NO_MEMORY,
    IDX_ERR_IO,
    IDX_ERR_PARSE,
    IDX_ERR_NOT_FOUND,
    IDX_ERR_RANGE,
    IDX_ERR_INTERNAL,
    /* Transport. IDX_ERR_TIMEOUT and IDX_ERR_CLOSED are expected outcomes on a
     * long-lived connection, not failures: callers retry or reconnect. */
    IDX_ERR_NETWORK,
    IDX_ERR_TIMEOUT,
    IDX_ERR_CLOSED,
    /* The peer answered, but with an error of its own (a JSON-RPC error
     * object, an HTTP status). Distinct from a transport failure because
     * retrying rarely helps. */
    IDX_ERR_REMOTE
} idx_status;

/* Stable, human-readable name for a status code. Never returns NULL. */
const char *idx_status_str(idx_status status);

#define IDX_ERROR_MSG_MAX 256

typedef struct {
    idx_status status;
    const char *file; /* NULL when no error has been recorded */
    int line;
    char message[IDX_ERROR_MSG_MAX];
} idx_error;

/* Resets `err` to the "no error" state. Safe to call with NULL. */
void idx_error_clear(idx_error *err);

/*
 * Records an error and returns `status` so call sites can write:
 *     return IDX_FAIL(err, IDX_ERR_PARSE, "bad slot: %s", text);
 * Passing IDX_OK is a programming error and is reported as IDX_ERR_INTERNAL.
 */
idx_status idx_error_setf(idx_error *err, idx_status status, const char *file,
                          int line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

#define IDX_FAIL(err, status, ...) \
    idx_error_setf((err), (status), __FILE__, __LINE__, __VA_ARGS__)

/* Propagates a failure to the caller unchanged. */
#define IDX_TRY(expr)                        \
    do {                                     \
        idx_status idx_try_status_ = (expr); \
        if (idx_try_status_ != IDX_OK) {     \
            return idx_try_status_;          \
        }                                    \
    } while (0)

#endif /* IDX_ERROR_H */
