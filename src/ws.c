#include "ws.h"

#include <curl/curl.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

#define IDX_WS_DEFAULT_CONNECT_TIMEOUT_MS 15000

/*
 * Blocks observed on mainnet have reached 9.6 MiB, and the ceiling moves with
 * chain activity. The cap only bounds a runaway peer, so it is set well clear
 * of anything seen rather than snugly above it: hitting it kills a connection
 * that was working fine.
 */
#define IDX_WS_DEFAULT_MAX_MESSAGE (64u * 1024u * 1024u)
#define IDX_WS_DEFAULT_INITIAL_BUFFER (16u * 1024u * 1024u)

/*
 * Size of a single curl_ws_recv call, and the receive buffer curl is asked
 * for. curl hands back at most one buffer's worth per call, so a small value
 * here turns a 9 MiB notification into hundreds of calls.
 */
#define IDX_WS_CHUNK (512u * 1024u)

struct idx_ws {
    CURL *curl;
    idx_buffer message;  /* the message being reassembled */
    uint8_t *chunk;      /* scratch for one curl_ws_recv */
    size_t max_message;
    idx_ws_stats stats;
    bool closed;
    /* Set once a complete message has been handed out, so the next call knows
     * to start a new one rather than append to the delivered bytes. */
    bool message_delivered;
};

static idx_status wait_ready(idx_ws *ws, short events, int timeout_ms,
                             idx_error *err);

void idx_ws_options_init(idx_ws_options *options) {
    if (options == NULL) {
        return;
    }
    options->url = NULL;
    options->connect_timeout_ms = IDX_WS_DEFAULT_CONNECT_TIMEOUT_MS;
    options->max_message_bytes = IDX_WS_DEFAULT_MAX_MESSAGE;
    options->initial_buffer_bytes = IDX_WS_DEFAULT_INITIAL_BUFFER;
    options->verify_tls = true;
}

/*
 * libcurl is initialized once per process. Doing it lazily here keeps callers
 * from having to remember, at the cost of not being safe to first-call from
 * two threads at once — which is why connections are established before the
 * pipeline forks its workers.
 */
static idx_status ensure_curl_global(idx_error *err) {
    static bool initialized = false;
    if (initialized) {
        return IDX_OK;
    }
    CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL, "curl_global_init failed: %s",
                        curl_easy_strerror(code));
    }
    initialized = true;
    return IDX_OK;
}

idx_status idx_ws_connect(const idx_ws_options *options, idx_ws **out,
                          idx_error *err) {
    if (options == NULL || out == NULL || options->url == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "options, options->url and out must not be NULL");
    }
    IDX_TRY(ensure_curl_global(err));

    idx_ws *ws = calloc(1, sizeof(*ws));
    if (ws == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to allocate a WebSocket handle");
    }
    idx_buffer_init(&ws->message);
    ws->max_message = (options->max_message_bytes > 0)
                          ? options->max_message_bytes
                          : IDX_WS_DEFAULT_MAX_MESSAGE;

    ws->chunk = malloc(IDX_WS_CHUNK);
    if (ws->chunk == NULL) {
        free(ws);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to allocate the WebSocket read buffer");
    }

    size_t initial = (options->initial_buffer_bytes > 0)
                         ? options->initial_buffer_bytes
                         : IDX_WS_DEFAULT_INITIAL_BUFFER;
    if (idx_buffer_reserve(&ws->message, initial, err) != IDX_OK) {
        free(ws->chunk);
        free(ws);
        return IDX_ERR_NO_MEMORY;
    }

    ws->curl = curl_easy_init();
    if (ws->curl == NULL) {
        idx_buffer_free(&ws->message);
        free(ws->chunk);
        free(ws);
        return IDX_FAIL(err, IDX_ERR_INTERNAL, "curl_easy_init failed");
    }

    long connect_timeout = (options->connect_timeout_ms > 0)
                               ? options->connect_timeout_ms
                               : IDX_WS_DEFAULT_CONNECT_TIMEOUT_MS;

    curl_easy_setopt(ws->curl, CURLOPT_URL, options->url);
    /* 2L puts the handle in WebSocket mode: curl performs the upgrade and then
     * hands the framing API over to us. */
    curl_easy_setopt(ws->curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(ws->curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout);
    curl_easy_setopt(ws->curl, CURLOPT_SSL_VERIFYPEER,
                     options->verify_tls ? 1L : 0L);
    curl_easy_setopt(ws->curl, CURLOPT_SSL_VERIFYHOST,
                     options->verify_tls ? 2L : 0L);
    curl_easy_setopt(ws->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(ws->curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(ws->curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(ws->curl, CURLOPT_USERAGENT, "solana_c_indexer/0.1");
    /* curl clamps this to its own maximum; asking for more is harmless and
     * keeps the fragment count down on multi-megabyte notifications. */
    curl_easy_setopt(ws->curl, CURLOPT_BUFFERSIZE, (long)IDX_WS_CHUNK);

    CURLcode code = curl_easy_perform(ws->curl);
    if (code != CURLE_OK) {
        idx_status status =
            IDX_FAIL(err, IDX_ERR_NETWORK, "WebSocket connect failed: %s",
                     curl_easy_strerror(code));
        curl_easy_cleanup(ws->curl);
        idx_buffer_free(&ws->message);
        free(ws->chunk);
        free(ws);
        return status;
    }

    IDX_DEBUG("websocket connected to %s", options->url);
    *out = ws;
    return IDX_OK;
}

void idx_ws_close(idx_ws *ws) {
    if (ws == NULL) {
        return;
    }
    if (ws->curl != NULL) {
        if (!ws->closed) {
            size_t sent = 0;
            /* Best effort: the peer may already be gone. */
            (void)curl_ws_send(ws->curl, "", 0, &sent, 0, CURLWS_CLOSE);
        }
        curl_easy_cleanup(ws->curl);
    }
    idx_buffer_free(&ws->message);
    free(ws->chunk);
    free(ws);
}

idx_status idx_ws_send_text(idx_ws *ws, idx_slice text, idx_error *err) {
    if (ws == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "ws must not be NULL");
    }
    if (ws->closed) {
        return IDX_FAIL(err, IDX_ERR_CLOSED, "the connection is closed");
    }

    size_t total = 0;
    while (total < text.len) {
        size_t sent = 0;
        CURLcode code = curl_ws_send(ws->curl, text.data + total,
                                     text.len - total, &sent, 0, CURLWS_TEXT);
        if (code == CURLE_AGAIN) {
            /* Wait for room rather than spinning on the socket. */
            idx_status waited = wait_ready(ws, POLLOUT, 5000, err);
            if (waited == IDX_ERR_TIMEOUT) {
                return IDX_FAIL(err, IDX_ERR_TIMEOUT,
                                "timed out sending %zu bytes", text.len);
            }
            IDX_TRY(waited);
            continue;
        }
        if (code != CURLE_OK) {
            return IDX_FAIL(err, IDX_ERR_NETWORK, "WebSocket send failed: %s",
                            curl_easy_strerror(code));
        }
        total += sent;
    }
    return IDX_OK;
}

/* Waits for the connection to become readable or writable. */
static idx_status wait_ready(idx_ws *ws, short events, int timeout_ms,
                             idx_error *err) {
    curl_socket_t socket = CURL_SOCKET_BAD;
    CURLcode code =
        curl_easy_getinfo(ws->curl, CURLINFO_ACTIVESOCKET, &socket);
    if (code != CURLE_OK || socket == CURL_SOCKET_BAD) {
        return IDX_FAIL(err, IDX_ERR_NETWORK,
                        "cannot obtain the WebSocket socket");
    }

    struct pollfd descriptor;
    descriptor.fd = (int)socket;
    descriptor.events = events;
    descriptor.revents = 0;

    int ready = poll(&descriptor, 1, timeout_ms);
    if (ready < 0) {
        /*
         * poll is never restarted after a signal, whatever SA_RESTART says, so
         * a process that handles signals at all will land here. It means "not
         * ready yet", not a broken connection: the caller re-polls against its
         * own deadline.
         */
        if (errno == EINTR) {
            return IDX_OK;
        }
        return IDX_FAIL(err, IDX_ERR_NETWORK, "poll on the WebSocket failed");
    }
    if (ready == 0) {
        return IDX_ERR_TIMEOUT;
    }
    return IDX_OK;
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

idx_status idx_ws_recv(idx_ws *ws, int timeout_ms, idx_slice *message,
                       idx_error *err) {
    if (ws == NULL || message == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "ws and message must not be NULL");
    }
    if (ws->closed) {
        return IDX_FAIL(err, IDX_ERR_CLOSED, "the connection is closed");
    }

    /*
     * The previous message's bytes are only dropped now, which is what lets
     * the slice handed out last time stay valid until this call. A message
     * still being reassembled is left alone, so a caller polling with a short
     * timeout still accumulates it across calls.
     */
    if (ws->message_delivered) {
        idx_buffer_clear(&ws->message);
        ws->message_delivered = false;
    }

    int64_t deadline = monotonic_ms() + timeout_ms;

    for (;;) {
        size_t received = 0;
        const struct curl_ws_frame *meta = NULL;
        CURLcode code = curl_ws_recv(ws->curl, ws->chunk, IDX_WS_CHUNK,
                                     &received, &meta);

        if (code == CURLE_AGAIN) {
            int64_t remaining = deadline - monotonic_ms();
            if (remaining <= 0) {
                /* Partial data stays in the buffer for the next call. */
                return IDX_ERR_TIMEOUT;
            }
            idx_status waited = wait_ready(ws, POLLIN, (int)remaining, err);
            if (waited == IDX_ERR_TIMEOUT) {
                return IDX_ERR_TIMEOUT;
            }
            IDX_TRY(waited);
            continue;
        }

        if (code == CURLE_GOT_NOTHING) {
            ws->closed = true;
            return IDX_FAIL(err, IDX_ERR_CLOSED,
                            "the peer closed the connection");
        }
        if (code != CURLE_OK) {
            return IDX_FAIL(err, IDX_ERR_NETWORK, "WebSocket receive failed: %s",
                            curl_easy_strerror(code));
        }
        if (meta == NULL) {
            return IDX_FAIL(err, IDX_ERR_INTERNAL,
                            "curl returned a frame without metadata");
        }

        if (meta->flags & CURLWS_CLOSE) {
            ws->closed = true;
            return IDX_FAIL(err, IDX_ERR_CLOSED, "the peer sent a close frame");
        }

        /*
         * Control frames carry no message payload. libcurl answers pings
         * itself, so they are only counted.
         */
        if (meta->flags & CURLWS_PING) {
            ws->stats.pings_received++;
            IDX_TRACE("websocket ping received");
            continue;
        }
        if (meta->flags & CURLWS_PONG) {
            continue;
        }

        if (received > 0) {
            if (ws->message.len + received > ws->max_message) {
                ws->closed = true;
                return IDX_FAIL(err, IDX_ERR_RANGE,
                                "message exceeds the %zu byte limit",
                                ws->max_message);
            }
            IDX_TRY(idx_buffer_append(&ws->message, ws->chunk, received, err));
            ws->stats.fragments_received++;
            ws->stats.bytes_received += received;
        }

        /*
         * The message is complete when nothing remains of the current frame
         * and the frame was not marked as continued.
         */
        if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
            ws->stats.messages_received++;
            if (ws->message.len > ws->stats.largest_message) {
                ws->stats.largest_message = ws->message.len;
            }
            ws->message_delivered = true;
            *message = idx_buffer_slice(&ws->message);
            return IDX_OK;
        }
    }
}

void idx_ws_get_stats(const idx_ws *ws, idx_ws_stats *out) {
    if (ws == NULL || out == NULL) {
        return;
    }
    *out = ws->stats;
}

size_t idx_ws_buffer_capacity(const idx_ws *ws) {
    return (ws != NULL) ? ws->message.capacity : 0;
}
