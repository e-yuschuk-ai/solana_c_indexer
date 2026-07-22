/*
 * Leveled logging.
 *
 * Output goes to a single sink (stderr by default) and is serialized with a
 * mutex, so it stays usable once the ingestion pipeline becomes concurrent.
 * Use the IDX_ERROR/IDX_WARN/... macros rather than idx_log_write directly:
 * they skip argument evaluation when the level is disabled.
 */
#ifndef IDX_LOG_H
#define IDX_LOG_H

#include <stdio.h>

#include "error.h"

typedef enum {
    IDX_LOG_ERROR = 0,
    IDX_LOG_WARN,
    IDX_LOG_INFO,
    IDX_LOG_DEBUG,
    IDX_LOG_TRACE
} idx_log_level;

/* Installs the sink and threshold. Passing NULL keeps the current sink. */
void idx_log_init(FILE *sink, idx_log_level level);
void idx_log_set_level(idx_log_level level);
idx_log_level idx_log_get_level(void);

/* Lowercase name of a level ("info"); never returns NULL. */
const char *idx_log_level_name(idx_log_level level);

/* Parses "error"/"warn"/"info"/"debug"/"trace", case-insensitively. */
idx_status idx_log_level_parse(const char *name, idx_log_level *out);

void idx_log_write(idx_log_level level, const char *file, int line,
                   const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#define IDX_LOG(level, ...)                                       \
    do {                                                          \
        if ((level) <= idx_log_get_level()) {                     \
            idx_log_write((level), __FILE__, __LINE__, __VA_ARGS__); \
        }                                                         \
    } while (0)

#define IDX_ERROR(...) IDX_LOG(IDX_LOG_ERROR, __VA_ARGS__)
#define IDX_WARN(...) IDX_LOG(IDX_LOG_WARN, __VA_ARGS__)
#define IDX_INFO(...) IDX_LOG(IDX_LOG_INFO, __VA_ARGS__)
#define IDX_DEBUG(...) IDX_LOG(IDX_LOG_DEBUG, __VA_ARGS__)
#define IDX_TRACE(...) IDX_LOG(IDX_LOG_TRACE, __VA_ARGS__)

#endif /* IDX_LOG_H */
