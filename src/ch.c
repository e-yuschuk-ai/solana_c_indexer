/*
 * ClickHouse HTTP client over libcurl. See include/ch.h for the contract.
 */
#include "ch.h"

#include <curl/curl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* A growable response buffer, kept NUL-terminated past `len`. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ch_buf;

struct idx_ch_conn {
    CURL *curl;
    struct curl_slist *headers;
    char *base_url; /* without a trailing slash */
    char *database; /* NULL when none was configured */
    ch_buf response;
    int exception_code; /* from the last request; 0 on success */
    long timeout_ms;
    long connect_timeout_ms;
};

struct idx_ch_result {
    char *data;
    size_t len;
};

static bool buf_reserve(ch_buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) {
        return true;
    }
    size_t want = b->cap ? b->cap : 4096;
    while (want < b->len + extra + 1) {
        want *= 2;
    }
    char *grown = realloc(b->data, want);
    if (grown == NULL) {
        return false;
    }
    b->data = grown;
    b->cap = want;
    return true;
}

static size_t on_data(char *ptr, size_t size, size_t nmemb, void *userdata) {
    idx_ch_conn *c = userdata;
    size_t n = size * nmemb;
    if (!buf_reserve(&c->response, n)) {
        return 0; /* aborts the transfer with CURLE_WRITE_ERROR */
    }
    memcpy(c->response.data + c->response.len, ptr, n);
    c->response.len += n;
    c->response.data[c->response.len] = '\0';
    return n;
}

/* Captures ClickHouse's exception-code header as it streams past. */
static size_t on_header(char *buffer, size_t size, size_t nitems,
                        void *userdata) {
    idx_ch_conn *c = userdata;
    size_t n = size * nitems;
    static const char key[] = "X-ClickHouse-Exception-Code:";
    size_t klen = sizeof key - 1;
    if (n > klen && strncasecmp(buffer, key, klen) == 0) {
        c->exception_code = (int)strtol(buffer + klen, NULL, 10);
    }
    return n;
}

void idx_ch_options_init(idx_ch_options *options) {
    if (options == NULL) {
        return;
    }
    options->url = NULL;
    options->database = NULL;
    options->user = NULL;
    options->password = NULL;
    options->timeout_ms = 30000;
    options->connect_timeout_ms = 5000;
}

/* Duplicates `s`, dropping a single trailing '/'. */
static char *dup_base_url(const char *s) {
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '/') {
        n--;
    }
    char *copy = malloc(n + 1);
    if (copy != NULL) {
        memcpy(copy, s, n);
        copy[n] = '\0';
    }
    return copy;
}

idx_status idx_ch_open(const idx_ch_options *options, idx_ch_conn **out,
                       idx_error *err) {
    if (options == NULL || options->url == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "ch open: null argument");
    }
    idx_ch_conn *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "ch open: allocation failed");
    }
    c->base_url = dup_base_url(options->url);
    if (options->database != NULL && options->database[0] != '\0') {
        c->database = strdup(options->database);
    }
    c->curl = curl_easy_init();
    if (c->base_url == NULL || c->curl == NULL) {
        idx_ch_close(c);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "ch open: allocation failed");
    }

    /* Credentials as headers rather than URL params, so they stay out of any
     * query-log URL. */
    char header[512];
    snprintf(header, sizeof header, "X-ClickHouse-User: %s",
             options->user != NULL ? options->user : "default");
    c->headers = curl_slist_append(c->headers, header);
    snprintf(header, sizeof header, "X-ClickHouse-Key: %s",
             options->password != NULL ? options->password : "");
    c->headers = curl_slist_append(c->headers, header);

    c->timeout_ms = options->timeout_ms > 0 ? options->timeout_ms : 30000;
    c->connect_timeout_ms =
        options->connect_timeout_ms > 0 ? options->connect_timeout_ms : 5000;

    curl_easy_setopt(c->curl, CURLOPT_HTTPHEADER, c->headers);
    curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, c);
    curl_easy_setopt(c->curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c->curl, CURLOPT_HEADERDATA, c);
    curl_easy_setopt(c->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c->curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c->curl, CURLOPT_USERAGENT, "solana_c_indexer/0.1");
    curl_easy_setopt(c->curl, CURLOPT_CONNECTTIMEOUT_MS, c->connect_timeout_ms);
    curl_easy_setopt(c->curl, CURLOPT_TIMEOUT_MS, c->timeout_ms);
    /* The response path may see gzip if the server is configured for it, and it
     * is nearly free to accept. */
    curl_easy_setopt(c->curl, CURLOPT_ACCEPT_ENCODING, "");

    *out = c;
    return IDX_OK;
}

void idx_ch_close(idx_ch_conn *conn) {
    if (conn == NULL) {
        return;
    }
    if (conn->curl != NULL) {
        curl_easy_cleanup(conn->curl);
    }
    if (conn->headers != NULL) {
        curl_slist_free_all(conn->headers);
    }
    free(conn->base_url);
    free(conn->database);
    free(conn->response.data);
    free(conn);
}

int idx_ch_exception_code(const idx_ch_conn *conn) {
    return conn != NULL ? conn->exception_code : 0;
}

/* The first line of the response, copied into `out` for an error message. */
static void first_line(const ch_buf *b, char *out, size_t n) {
    if (n == 0) {
        return;
    }
    size_t i = 0;
    while (b->data != NULL && i < b->len && b->data[i] != '\n' && i + 1 < n) {
        out[i] = b->data[i];
        i++;
    }
    out[i] = '\0';
}

/*
 * Performs a request: `url` fully built, `post` selecting POST vs GET, `body`
 * the POST payload. Maps the outcome to an idx_status. On success the caller
 * owns conn->response until the next request.
 */
static idx_status perform(idx_ch_conn *conn, const char *url, bool post,
                          const void *body, size_t body_len, const char *what,
                          idx_error *err) {
    conn->response.len = 0;
    if (conn->response.data != NULL) {
        conn->response.data[0] = '\0';
    }
    conn->exception_code = 0;

    curl_easy_setopt(conn->curl, CURLOPT_URL, url);
    if (post) {
        curl_easy_setopt(conn->curl, CURLOPT_POST, 1L);
        curl_easy_setopt(conn->curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(conn->curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    } else {
        curl_easy_setopt(conn->curl, CURLOPT_HTTPGET, 1L);
    }

    CURLcode code = curl_easy_perform(conn->curl);
    if (code != CURLE_OK) {
        idx_status status = code == CURLE_OPERATION_TIMEDOUT ? IDX_ERR_TIMEOUT
                                                             : IDX_ERR_NETWORK;
        return IDX_FAIL(err, status, "%s: %s", what, curl_easy_strerror(code));
    }

    long http = 0;
    curl_easy_getinfo(conn->curl, CURLINFO_RESPONSE_CODE, &http);
    if (http >= 400) {
        char msg[256];
        first_line(&conn->response, msg, sizeof msg);
        return IDX_FAIL(err, IDX_ERR_REMOTE, "%s: http %ld [code %d] %s", what,
                        http, conn->exception_code, msg);
    }
    return IDX_OK;
}

idx_status idx_ch_ping(idx_ch_conn *conn, idx_error *err) {
    if (conn == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "ch ping: null connection");
    }
    char url[512];
    snprintf(url, sizeof url, "%s/ping", conn->base_url);
    idx_status st = perform(conn, url, false, NULL, 0, "ch ping", err);
    if (st != IDX_OK) {
        return st;
    }
    /* A healthy server answers "Ok.\n". */
    if (conn->response.len < 2 || strncmp(conn->response.data, "Ok", 2) != 0) {
        return IDX_FAIL(err, IDX_ERR_NETWORK, "ch ping: unexpected reply");
    }
    return IDX_OK;
}

/* Moves conn->response into a fresh result, or discards it when out is NULL. */
static idx_status take_result(idx_ch_conn *conn, idx_ch_result **out,
                              idx_error *err) {
    if (out == NULL) {
        return IDX_OK;
    }
    idx_ch_result *r = malloc(sizeof *r);
    char *body = malloc(conn->response.len + 1);
    if (r == NULL || body == NULL) {
        free(r);
        free(body);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "ch result: allocation failed");
    }
    memcpy(body, conn->response.data, conn->response.len);
    body[conn->response.len] = '\0';
    r->data = body;
    r->len = conn->response.len;
    *out = r;
    return IDX_OK;
}

/* Appends "?database=<db>" (escaped) to `url` when a database is configured,
 * returning the offset past what was written. `sep` is '?' for the first
 * param. */
static size_t append_database(idx_ch_conn *conn, char *url, size_t cap,
                              size_t at, char sep) {
    if (conn->database == NULL) {
        return at;
    }
    char *escaped = curl_easy_escape(conn->curl, conn->database, 0);
    if (escaped == NULL) {
        return at;
    }
    int wrote = snprintf(url + at, cap - at, "%cdatabase=%s", sep, escaped);
    curl_free(escaped);
    if (wrote > 0 && (size_t)wrote < cap - at) {
        at += (size_t)wrote;
    }
    return at;
}

idx_status idx_ch_query(idx_ch_conn *conn, const char *sql,
                        idx_ch_result **out, idx_error *err) {
    if (conn == NULL || sql == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "ch query: null argument");
    }
    char url[1024];
    size_t at = (size_t)snprintf(url, sizeof url, "%s/", conn->base_url);
    append_database(conn, url, sizeof url, at, '?');

    idx_status st =
        perform(conn, url, true, sql, strlen(sql), "ch query", err);
    if (st != IDX_OK) {
        return st;
    }
    return take_result(conn, out, err);
}

idx_status idx_ch_insert(idx_ch_conn *conn, const char *sql, const void *data,
                         size_t len, idx_error *err) {
    if (conn == NULL || sql == NULL || (data == NULL && len != 0)) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "ch insert: bad argument");
    }
    char *escaped = curl_easy_escape(conn->curl, sql, 0);
    if (escaped == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "ch insert: escape failed");
    }
    /* The statement rides in the URL so the body is exactly the data. */
    char url[4096];
    size_t at = (size_t)snprintf(url, sizeof url, "%s/", conn->base_url);
    at = append_database(conn, url, sizeof url, at, '?');
    char sep = conn->database != NULL ? '&' : '?';
    snprintf(url + at, sizeof url - at, "%cquery=%s", sep, escaped);
    curl_free(escaped);

    return perform(conn, url, true, data != NULL ? data : "", len, "ch insert",
                   err);
}

void idx_ch_result_free(idx_ch_result *res) {
    if (res != NULL) {
        free(res->data);
        free(res);
    }
}

const char *idx_ch_result_body(const idx_ch_result *res) {
    return res != NULL ? res->data : NULL;
}

size_t idx_ch_result_len(const idx_ch_result *res) {
    return res != NULL ? res->len : 0;
}
