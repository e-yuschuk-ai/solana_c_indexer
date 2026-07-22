#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log_sink = NULL; /* resolved to stderr on first use */
static idx_log_level g_log_level = IDX_LOG_INFO;

static const char *const k_level_names[] = {"error", "warn", "info", "debug",
                                            "trace"};
static const size_t k_level_count =
    sizeof(k_level_names) / sizeof(k_level_names[0]);

void idx_log_init(FILE *sink, idx_log_level level) {
    pthread_mutex_lock(&g_log_mutex);
    if (sink != NULL) {
        g_log_sink = sink;
    }
    g_log_level = level;
    pthread_mutex_unlock(&g_log_mutex);
}

void idx_log_set_level(idx_log_level level) {
    pthread_mutex_lock(&g_log_mutex);
    g_log_level = level;
    pthread_mutex_unlock(&g_log_mutex);
}

idx_log_level idx_log_get_level(void) {
    pthread_mutex_lock(&g_log_mutex);
    idx_log_level level = g_log_level;
    pthread_mutex_unlock(&g_log_mutex);
    return level;
}

const char *idx_log_level_name(idx_log_level level) {
    size_t index = (size_t)level;
    if (index >= k_level_count) {
        return "unknown";
    }
    return k_level_names[index];
}

idx_status idx_log_level_parse(const char *name, idx_log_level *out) {
    if (name == NULL || out == NULL) {
        return IDX_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < k_level_count; i++) {
        if (strcasecmp(name, k_level_names[i]) == 0) {
            *out = (idx_log_level)i;
            return IDX_OK;
        }
    }
    /* "warning" is common enough to be worth accepting. */
    if (strcasecmp(name, "warning") == 0) {
        *out = IDX_LOG_WARN;
        return IDX_OK;
    }
    return IDX_ERR_PARSE;
}

/* Writes "2026-07-22 14:03:11.482" into `buf`. */
static void format_timestamp(char *buf, size_t size) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        snprintf(buf, size, "-");
        return;
    }

    struct tm tm_buf;
    if (localtime_r(&ts.tv_sec, &tm_buf) == NULL) {
        snprintf(buf, size, "-");
        return;
    }

    char seconds[20];
    strftime(seconds, sizeof(seconds), "%Y-%m-%d %H:%M:%S", &tm_buf);

    /* Clamped so the compiler can bound the width of the %03u field. */
    long millis = ts.tv_nsec / 1000000L;
    if (millis < 0) {
        millis = 0;
    } else if (millis > 999) {
        millis = 999;
    }
    snprintf(buf, size, "%s.%03u", seconds, (unsigned)millis);
}

/* Trims the leading directories so log lines stay narrow. */
static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

void idx_log_write(idx_log_level level, const char *file, int line,
                   const char *fmt, ...) {
    char timestamp[32];
    format_timestamp(timestamp, sizeof(timestamp));

    char message[1024];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    if (written < 0) {
        snprintf(message, sizeof(message), "<malformed log message>");
    }

    pthread_mutex_lock(&g_log_mutex);
    FILE *sink = (g_log_sink != NULL) ? g_log_sink : stderr;
    fprintf(sink, "%s %-5s %s:%d: %s\n", timestamp, idx_log_level_name(level),
            basename_of(file), line, message);
    fflush(sink);
    pthread_mutex_unlock(&g_log_mutex);
}
