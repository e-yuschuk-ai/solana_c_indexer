#include "pubsub.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "vec.h"
#include "ws.h"

#define IDX_PUBSUB_DEFAULT_BACKOFF_INITIAL_MS 500
#define IDX_PUBSUB_DEFAULT_BACKOFF_MAX_MS 30000
#define IDX_PUBSUB_DEFAULT_IDLE_TIMEOUT_MS 60000

typedef struct {
    uint64_t handle;      /* what the caller holds */
    uint64_t request_id;  /* id of the outstanding subscribe request */
    uint64_t server_id;   /* what the server assigned, 0 until confirmed */
    char *method;
    char *params;
    bool confirmed;
} subscription;

struct idx_pubsub {
    idx_pubsub_options options;
    char *url; /* owned copy: the caller's string need not outlive us */

    idx_ws *ws;
    idx_vec subscriptions; /* of subscription */

    uint64_t next_handle;
    uint64_t next_request_id;

    int backoff_ms;
    int64_t last_message_ms;

    idx_pubsub_stats stats;
    uint32_t rng_state;
};

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void idx_pubsub_options_init(idx_pubsub_options *options) {
    if (options == NULL) {
        return;
    }
    options->url = NULL;
    options->reconnect_initial_ms = IDX_PUBSUB_DEFAULT_BACKOFF_INITIAL_MS;
    options->reconnect_max_ms = IDX_PUBSUB_DEFAULT_BACKOFF_MAX_MS;
    options->idle_timeout_ms = IDX_PUBSUB_DEFAULT_IDLE_TIMEOUT_MS;
    options->max_message_bytes = 0; /* let idx_ws choose */
    options->verify_tls = true;
}

/* xorshift32, so backoff jitter does not disturb the global rand() state. */
static uint32_t next_random(idx_pubsub *pubsub) {
    uint32_t x = pubsub->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    pubsub->rng_state = x;
    return x;
}

static subscription *subscription_at(idx_pubsub *pubsub, size_t index) {
    return idx_vec_at(&pubsub->subscriptions, index);
}

static void subscription_release(subscription *entry) {
    free(entry->method);
    free(entry->params);
    entry->method = NULL;
    entry->params = NULL;
}

/* Sends one subscribe request and records the id it is waiting on. */
static idx_status send_subscribe(idx_pubsub *pubsub, subscription *entry,
                                 idx_error *err) {
    idx_buffer request;
    idx_buffer_init(&request);

    entry->request_id = pubsub->next_request_id++;
    entry->server_id = 0;
    entry->confirmed = false;

    idx_status status = idx_json_write_rpc_request(
        &request, entry->request_id, entry->method, entry->params, err);
    if (status == IDX_OK) {
        status = idx_ws_send_text(pubsub->ws, idx_buffer_slice(&request), err);
    }
    idx_buffer_free(&request);

    if (status == IDX_OK) {
        IDX_DEBUG("sent %s as request %llu", entry->method,
                  (unsigned long long)entry->request_id);
    }
    return status;
}

static void drop_connection(idx_pubsub *pubsub) {
    if (pubsub->ws != NULL) {
        idx_ws_close(pubsub->ws);
        pubsub->ws = NULL;
    }
    pubsub->stats.connected = false;

    for (size_t i = 0; i < idx_vec_len(&pubsub->subscriptions); i++) {
        subscription *entry = subscription_at(pubsub, i);
        entry->confirmed = false;
        entry->server_id = 0;
    }
}

static idx_status establish(idx_pubsub *pubsub, idx_error *err) {
    idx_ws_options ws_options;
    idx_ws_options_init(&ws_options);
    ws_options.url = pubsub->url;
    ws_options.verify_tls = pubsub->options.verify_tls;
    if (pubsub->options.max_message_bytes > 0) {
        ws_options.max_message_bytes = pubsub->options.max_message_bytes;
    }

    IDX_TRY(idx_ws_connect(&ws_options, &pubsub->ws, err));

    pubsub->stats.connected = true;
    pubsub->last_message_ms = monotonic_ms();
    pubsub->backoff_ms = pubsub->options.reconnect_initial_ms;

    /* Everything registered so far is re-established before returning, so a
     * caller never observes a connected-but-unsubscribed state. */
    for (size_t i = 0; i < idx_vec_len(&pubsub->subscriptions); i++) {
        idx_status status = send_subscribe(pubsub, subscription_at(pubsub, i),
                                           err);
        if (status != IDX_OK) {
            drop_connection(pubsub);
            return status;
        }
    }
    return IDX_OK;
}

idx_status idx_pubsub_open(const idx_pubsub_options *options, idx_pubsub **out,
                           idx_error *err) {
    if (options == NULL || out == NULL || options->url == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "options, options->url and out must not be NULL");
    }

    idx_pubsub *pubsub = calloc(1, sizeof(*pubsub));
    if (pubsub == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to allocate the PubSub client");
    }

    pubsub->options = *options;
    pubsub->url = strdup(options->url);
    if (pubsub->url == NULL) {
        free(pubsub);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "failed to copy the URL");
    }
    pubsub->options.url = pubsub->url;

    if (pubsub->options.reconnect_initial_ms <= 0) {
        pubsub->options.reconnect_initial_ms =
            IDX_PUBSUB_DEFAULT_BACKOFF_INITIAL_MS;
    }
    if (pubsub->options.reconnect_max_ms < pubsub->options.reconnect_initial_ms) {
        pubsub->options.reconnect_max_ms = pubsub->options.reconnect_initial_ms;
    }

    idx_vec_init(&pubsub->subscriptions, sizeof(subscription));
    pubsub->next_handle = 1;
    pubsub->next_request_id = 1;
    pubsub->backoff_ms = pubsub->options.reconnect_initial_ms;
    pubsub->rng_state = (uint32_t)monotonic_ms() | 1u;

    idx_status status = establish(pubsub, err);
    if (status != IDX_OK) {
        idx_vec_free(&pubsub->subscriptions);
        free(pubsub->url);
        free(pubsub);
        return status;
    }

    *out = pubsub;
    return IDX_OK;
}

void idx_pubsub_close(idx_pubsub *pubsub) {
    if (pubsub == NULL) {
        return;
    }
    drop_connection(pubsub);
    for (size_t i = 0; i < idx_vec_len(&pubsub->subscriptions); i++) {
        subscription_release(subscription_at(pubsub, i));
    }
    idx_vec_free(&pubsub->subscriptions);
    free(pubsub->url);
    free(pubsub);
}

idx_status idx_pubsub_subscribe(idx_pubsub *pubsub, const char *method,
                                const char *params_json, uint64_t *handle,
                                idx_error *err) {
    if (pubsub == NULL || method == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "pubsub and method must not be NULL");
    }

    subscription entry;
    memset(&entry, 0, sizeof(entry));
    entry.handle = pubsub->next_handle++;
    entry.method = strdup(method);
    entry.params = (params_json != NULL) ? strdup(params_json) : NULL;

    if (entry.method == NULL || (params_json != NULL && entry.params == NULL)) {
        subscription_release(&entry);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to copy the subscription");
    }

    idx_status pushed = idx_vec_push(&pubsub->subscriptions, &entry, err);
    if (pushed != IDX_OK) {
        subscription_release(&entry);
        return pushed;
    }
    subscription *stored =
        subscription_at(pubsub, idx_vec_len(&pubsub->subscriptions) - 1);

    if (pubsub->ws != NULL) {
        idx_status status = send_subscribe(pubsub, stored, err);
        if (status != IDX_OK) {
            /* The registry keeps it; the next reconnect will try again. */
            drop_connection(pubsub);
        }
    }

    if (handle != NULL) {
        *handle = entry.handle;
    }
    return IDX_OK;
}

/* Backoff with full jitter, so reconnecting clients do not synchronize. */
static int next_backoff(idx_pubsub *pubsub) {
    int current = pubsub->backoff_ms;
    int jittered = (int)(next_random(pubsub) % (uint32_t)(current / 2 + 1)) +
                   current / 2;

    if (pubsub->backoff_ms < pubsub->options.reconnect_max_ms / 2) {
        pubsub->backoff_ms *= 2;
    } else {
        pubsub->backoff_ms = pubsub->options.reconnect_max_ms;
    }
    return jittered;
}

static void sleep_ms(int milliseconds) {
    struct timespec request;
    request.tv_sec = milliseconds / 1000;
    request.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&request, NULL);
}

static subscription *find_by_request(idx_pubsub *pubsub, uint64_t request_id) {
    for (size_t i = 0; i < idx_vec_len(&pubsub->subscriptions); i++) {
        subscription *entry = subscription_at(pubsub, i);
        if (!entry->confirmed && entry->request_id == request_id) {
            return entry;
        }
    }
    return NULL;
}

static subscription *find_by_server_id(idx_pubsub *pubsub, uint64_t server_id) {
    for (size_t i = 0; i < idx_vec_len(&pubsub->subscriptions); i++) {
        subscription *entry = subscription_at(pubsub, i);
        if (entry->confirmed && entry->server_id == server_id) {
            return entry;
        }
    }
    return NULL;
}

/*
 * Classifies one received message. Returns IDX_OK when `out` was filled with a
 * notification, IDX_ERR_NOT_FOUND when the message was consumed internally,
 * and IDX_ERR_REMOTE when the server rejected a subscription.
 */
static idx_status dispatch(idx_pubsub *pubsub, idx_slice raw,
                           idx_pubsub_message *out, idx_error *err) {
    idx_json_doc *doc = NULL;
    IDX_TRY(idx_json_parse(raw, &doc, err));

    idx_json_val root = idx_json_root(doc);

    /* A response carries "id"; a notification carries "method". */
    uint64_t request_id = 0;
    if (idx_json_opt_u64(root, "id", &request_id)) {
        idx_json_val error_object = idx_json_get(root, "error");
        if (idx_json_is_present(error_object) &&
            !idx_json_is_null(error_object)) {
            idx_slice text = idx_slice_from_str("(no message)");
            int64_t code = 0;
            idx_json_opt_string(error_object, "message", &text);
            idx_json_read_i64(error_object, "code", &code, NULL);

            subscription *entry = find_by_request(pubsub, request_id);
            pubsub->stats.subscribe_failures++;
            idx_status status = IDX_FAIL(
                err, IDX_ERR_REMOTE, "%s was rejected: %.*s (code %lld)",
                (entry != NULL) ? entry->method : "a request", (int)text.len,
                (const char *)text.data, (long long)code);
            idx_json_free(doc);
            return status;
        }

        subscription *entry = find_by_request(pubsub, request_id);
        uint64_t server_id = 0;
        if (entry != NULL && idx_json_opt_u64(root, "result", &server_id)) {
            entry->server_id = server_id;
            entry->confirmed = true;
            IDX_INFO("%s confirmed as subscription %llu", entry->method,
                     (unsigned long long)server_id);
        }
        idx_json_free(doc);
        return IDX_ERR_NOT_FOUND;
    }

    idx_json_val params = idx_json_get(root, "params");
    uint64_t server_id = 0;
    if (!idx_json_opt_u64(params, "subscription", &server_id)) {
        IDX_DEBUG("ignoring a message that is neither response nor notification");
        idx_json_free(doc);
        return IDX_ERR_NOT_FOUND;
    }

    subscription *entry = find_by_server_id(pubsub, server_id);
    if (entry == NULL) {
        /* Expected briefly after a reconnect, when notifications for the
         * previous subscription are still in flight. */
        pubsub->stats.unmatched++;
        idx_json_free(doc);
        return IDX_ERR_NOT_FOUND;
    }

    out->subscription = entry->handle;
    out->doc = doc;
    out->result = idx_json_get(params, "result");
    out->raw = raw;
    pubsub->stats.notifications++;
    return IDX_OK;
}

idx_status idx_pubsub_poll(idx_pubsub *pubsub, int timeout_ms,
                           idx_pubsub_message *out, idx_error *err) {
    if (pubsub == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "pubsub and out must not be NULL");
    }
    memset(out, 0, sizeof(*out));

    int64_t deadline = monotonic_ms() + timeout_ms;

    for (;;) {
        int64_t remaining = deadline - monotonic_ms();
        if (remaining <= 0) {
            return IDX_ERR_TIMEOUT;
        }

        if (pubsub->ws == NULL) {
            int delay = next_backoff(pubsub);
            if (delay > remaining) {
                /* Sleeping past the caller's deadline would hide the
                 * disconnection; report a timeout and let it call again. */
                sleep_ms((int)remaining);
                return IDX_ERR_TIMEOUT;
            }
            sleep_ms(delay);

            idx_error connect_err;
            idx_error_clear(&connect_err);
            if (establish(pubsub, &connect_err) != IDX_OK) {
                IDX_WARN("reconnect failed: %s", connect_err.message);
                continue;
            }
            pubsub->stats.reconnects++;
            IDX_INFO("reconnected after %d ms", delay);
            continue;
        }

        idx_slice raw;
        idx_status status =
            idx_ws_recv(pubsub->ws, (int)remaining, &raw, err);

        if (status == IDX_ERR_TIMEOUT) {
            int idle = pubsub->options.idle_timeout_ms;
            if (idle > 0 && monotonic_ms() - pubsub->last_message_ms > idle) {
                IDX_WARN("no message for %d ms, reconnecting", idle);
                drop_connection(pubsub);
                continue;
            }
            return IDX_ERR_TIMEOUT;
        }

        if (status == IDX_ERR_CLOSED || status == IDX_ERR_NETWORK) {
            IDX_WARN("connection lost: %s", (err != NULL) ? err->message : "");
            drop_connection(pubsub);
            continue;
        }
        if (status != IDX_OK) {
            return status;
        }

        pubsub->last_message_ms = monotonic_ms();

        idx_status dispatched = dispatch(pubsub, raw, out, err);
        if (dispatched == IDX_OK) {
            return IDX_OK;
        }
        if (dispatched == IDX_ERR_REMOTE) {
            return dispatched;
        }
        /* Consumed internally; keep waiting within the same budget. */
    }
}

void idx_pubsub_message_free(idx_pubsub_message *message) {
    if (message == NULL) {
        return;
    }
    idx_json_free(message->doc);
    /* `result` and `raw` borrowed the document and the connection buffer, so
     * the whole struct is cleared rather than left pointing at freed memory. */
    memset(message, 0, sizeof(*message));
}

void idx_pubsub_get_stats(const idx_pubsub *pubsub, idx_pubsub_stats *out) {
    if (pubsub == NULL || out == NULL) {
        return;
    }
    *out = pubsub->stats;
}
