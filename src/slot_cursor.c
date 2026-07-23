#include "slot_cursor.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IDX_SLOT_CURSOR_LINE_MAX 256

void idx_slot_cursor_init(idx_slot_cursor *cursor, idx_slot start_slot) {
    if (cursor == NULL) {
        return;
    }
    cursor->last_indexed = IDX_SLOT_NONE;
    cursor->last_seen = IDX_SLOT_NONE;
    cursor->start_slot = start_slot;
    cursor->path[0] = '\0';
}

void idx_slot_cursor_record_indexed(idx_slot_cursor *cursor, idx_slot slot) {
    if (cursor == NULL) {
        return;
    }
    if (cursor->last_indexed == IDX_SLOT_NONE || slot > cursor->last_indexed) {
        cursor->last_indexed = slot;
    }
}

void idx_slot_cursor_set_indexed(idx_slot_cursor *cursor, idx_slot slot) {
    if (cursor == NULL) {
        return;
    }
    cursor->last_indexed = slot;
}

void idx_slot_cursor_observe(idx_slot_cursor *cursor, idx_slot slot) {
    if (cursor == NULL) {
        return;
    }
    if (cursor->last_seen == IDX_SLOT_NONE || slot > cursor->last_seen) {
        cursor->last_seen = slot;
    }
}

idx_slot idx_slot_cursor_resume_slot(const idx_slot_cursor *cursor) {
    if (cursor == NULL) {
        return 0;
    }
    if (cursor->last_indexed != IDX_SLOT_NONE) {
        return cursor->last_indexed + 1;
    }
    return cursor->start_slot;
}

/* Trims ASCII whitespace in place and returns the first non-space character. */
static char *trim(char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    size_t len = strlen(text);
    while (len > 0) {
        char last = text[len - 1];
        if (last != ' ' && last != '\t' && last != '\r' && last != '\n') {
            break;
        }
        text[--len] = '\0';
    }
    return text;
}

static idx_status parse_slot(const char *value, idx_slot *out, const char *path,
                             int line, idx_error *err) {
    if (*value == '\0') {
        return IDX_FAIL(err, IDX_ERR_PARSE, "%s:%d: last_indexed is empty", path,
                        line);
    }
    /* strtoull silently wraps a leading '-'; reject it before parsing. */
    if (strchr(value, '-') != NULL) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "%s:%d: last_indexed = %s must not be negative", path,
                        line, value);
    }

    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno == ERANGE || parsed == IDX_SLOT_NONE) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "%s:%d: last_indexed = %s is out of range", path, line,
                        value);
    }
    if (end == value || *end != '\0') {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "%s:%d: last_indexed = %s is not a number", path, line,
                        value);
    }

    *out = (idx_slot)parsed;
    return IDX_OK;
}

idx_status idx_slot_cursor_load(idx_slot_cursor *cursor, const char *path,
                                idx_error *err) {
    if (cursor == NULL || path == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "cursor and path must not be NULL");
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return IDX_FAIL(err, IDX_ERR_NOT_FOUND,
                            "cursor file '%s' does not exist", path);
        }
        return IDX_FAIL(err, IDX_ERR_IO, "cannot open cursor file '%s': %s",
                        path, strerror(errno));
    }

    /* Absence of a last_indexed line means "no progress yet", not an error, so
     * a freshly created cursor file round-trips through save and load. */
    idx_slot last_indexed = IDX_SLOT_NONE;
    char line[IDX_SLOT_CURSOR_LINE_MAX];
    int line_number = 0;
    idx_status status = IDX_OK;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;

        if (strchr(line, '\n') == NULL && !feof(file)) {
            status = IDX_FAIL(err, IDX_ERR_PARSE, "%s:%d: line exceeds %d bytes",
                              path, line_number, IDX_SLOT_CURSOR_LINE_MAX - 1);
            break;
        }

        char *comment = strchr(line, '#');
        if (comment != NULL) {
            *comment = '\0';
        }

        char *content = trim(line);
        if (*content == '\0') {
            continue;
        }

        char *separator = strchr(content, '=');
        if (separator == NULL) {
            status = IDX_FAIL(err, IDX_ERR_PARSE,
                              "%s:%d: expected 'key = value'", path,
                              line_number);
            break;
        }
        *separator = '\0';

        char *key = trim(content);
        char *value = trim(separator + 1);

        if (strcmp(key, "last_indexed") == 0) {
            status = parse_slot(value, &last_indexed, path, line_number, err);
            if (status != IDX_OK) {
                break;
            }
        } else {
            status = IDX_FAIL(err, IDX_ERR_PARSE, "%s:%d: unknown key '%s'",
                              path, line_number, key);
            break;
        }
    }

    if (status == IDX_OK && ferror(file)) {
        status = IDX_FAIL(err, IDX_ERR_IO, "error reading '%s': %s", path,
                          strerror(errno));
    }

    fclose(file);
    if (status != IDX_OK) {
        return status;
    }

    cursor->last_indexed = last_indexed;
    return IDX_OK;
}

idx_status idx_slot_cursor_open(idx_slot_cursor *cursor, const char *path,
                                idx_slot start_slot, idx_error *err) {
    if (cursor == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "cursor must not be NULL");
    }

    idx_slot_cursor_init(cursor, start_slot);

    if (path == NULL || *path == '\0') {
        return IDX_OK;
    }

    size_t len = strlen(path);
    if (len >= sizeof(cursor->path)) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "cursor path is %zu bytes, maximum is %zu", len,
                        sizeof(cursor->path) - 1);
    }
    memcpy(cursor->path, path, len + 1);

    idx_status status = idx_slot_cursor_load(cursor, path, err);
    if (status == IDX_ERR_NOT_FOUND) {
        idx_error_clear(err); /* a missing file is a fresh start */
        return IDX_OK;
    }
    return status;
}

idx_status idx_slot_cursor_save(const idx_slot_cursor *cursor, idx_error *err) {
    if (cursor == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "cursor must not be NULL");
    }
    if (cursor->path[0] == '\0') {
        return IDX_OK; /* in-memory cursor: nothing to persist */
    }

    char tmp[IDX_SLOT_CURSOR_PATH_MAX + 8];
    int written = snprintf(tmp, sizeof(tmp), "%s.tmp", cursor->path);
    if (written < 0 || (size_t)written >= sizeof(tmp)) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "cursor path '%s' is too long to write", cursor->path);
    }

    FILE *file = fopen(tmp, "w");
    if (file == NULL) {
        return IDX_FAIL(err, IDX_ERR_IO, "cannot open '%s': %s", tmp,
                        strerror(errno));
    }

    int ok = fputs("# solana_c_indexer slot cursor\n", file) >= 0;
    if (ok && cursor->last_indexed != IDX_SLOT_NONE) {
        ok = fprintf(file, "last_indexed = %" PRIu64 "\n",
                     cursor->last_indexed) > 0;
    }

    /* Flush to disk before the rename so the swap can only expose a complete
     * file, never a truncated one. */
    if (ok) {
        ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    }
    if (fclose(file) != 0) {
        ok = 0;
    }

    if (!ok) {
        int write_errno = errno; /* remove() below may clobber errno */
        remove(tmp);
        return IDX_FAIL(err, IDX_ERR_IO, "cannot write cursor to '%s': %s", tmp,
                        strerror(write_errno));
    }

    if (rename(tmp, cursor->path) != 0) {
        int rename_errno = errno;
        remove(tmp);
        return IDX_FAIL(err, IDX_ERR_IO, "cannot rename '%s' to '%s': %s", tmp,
                        cursor->path, strerror(rename_errno));
    }

    return IDX_OK;
}
