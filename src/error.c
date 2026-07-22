#include "error.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *idx_status_str(idx_status status) {
    switch (status) {
        case IDX_OK:
            return "ok";
        case IDX_ERR_INVALID_ARG:
            return "invalid argument";
        case IDX_ERR_NO_MEMORY:
            return "out of memory";
        case IDX_ERR_IO:
            return "I/O error";
        case IDX_ERR_PARSE:
            return "parse error";
        case IDX_ERR_NOT_FOUND:
            return "not found";
        case IDX_ERR_RANGE:
            return "value out of range";
        case IDX_ERR_INTERNAL:
            return "internal error";
    }
    return "unknown error";
}

void idx_error_clear(idx_error *err) {
    if (err == NULL) {
        return;
    }
    err->status = IDX_OK;
    err->file = NULL;
    err->line = 0;
    err->message[0] = '\0';
}

idx_status idx_error_setf(idx_error *err, idx_status status, const char *file,
                          int line, const char *fmt, ...) {
    if (status == IDX_OK) {
        status = IDX_ERR_INTERNAL;
    }
    if (err == NULL) {
        return status;
    }

    err->status = status;
    err->file = file;
    err->line = line;

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(err->message, sizeof(err->message), fmt, args);
    va_end(args);

    if (written < 0) {
        /* Encoding failure: keep the status but fall back to a generic text. */
        snprintf(err->message, sizeof(err->message), "%s",
                 idx_status_str(status));
    }
    return status;
}
