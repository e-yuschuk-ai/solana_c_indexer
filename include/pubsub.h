/*
 * Solana JSON-RPC PubSub over the WebSocket transport.
 *
 * Owns everything idx_ws deliberately does not: the subscription registry,
 * request/response correlation, notification demultiplexing, and reconnection
 * with resubscription. A caller registers subscriptions once and then polls;
 * across a dropped connection the subscriptions are re-established without its
 * involvement.
 *
 * What a caller must still handle is the gap: a reconnect means slots were
 * missed, because the server does not replay. `idx_pubsub_stats.reconnects`
 * changing is the signal to go and fetch them over HTTP.
 *
 * Not thread-safe; one owner per instance.
 */
#ifndef IDX_PUBSUB_H
#define IDX_PUBSUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bytes.h"
#include "error.h"
#include "json.h"

typedef struct idx_pubsub idx_pubsub;

typedef struct {
    const char *url;

    /* Backoff bounds for reconnection. */
    int reconnect_initial_ms;
    int reconnect_max_ms;

    /*
     * Force a reconnect when nothing has arrived for this long. Solana
     * produces a block every ~400 ms, so a long silence means the connection
     * is dead in a way TCP has not noticed yet. 0 disables the check.
     */
    int idle_timeout_ms;

    size_t max_message_bytes;
    bool verify_tls;
} idx_pubsub_options;

/* Defaults: 500 ms to 30 s backoff, 60 s idle timeout, TLS verified. */
void idx_pubsub_options_init(idx_pubsub_options *options);

typedef struct {
    uint64_t reconnects;
    uint64_t notifications;
    uint64_t subscribe_failures;
    /* Notifications for a subscription id we do not know, which happens
     * briefly after a reconnect and is otherwise a bug. */
    uint64_t unmatched;
    bool connected;
} idx_pubsub_stats;

typedef struct {
    /* The handle returned by idx_pubsub_subscribe. */
    uint64_t subscription;

    /* Owned by the caller: release with idx_pubsub_message_free. */
    idx_json_doc *doc;

    /* `params.result` of the notification, borrowing `doc`. */
    idx_json_val result;

    /* The undecoded message. Borrows the connection's buffer and is only
     * valid until the next poll. */
    idx_slice raw;
} idx_pubsub_message;

/*
 * Connects. Subscriptions may be registered before or after this; anything
 * registered while disconnected is sent once the connection is up.
 */
idx_status idx_pubsub_open(const idx_pubsub_options *options, idx_pubsub **out,
                           idx_error *err);

void idx_pubsub_close(idx_pubsub *pubsub);

/*
 * Registers a subscription and returns its handle. `method` and `params_json`
 * are copied, and re-sent verbatim after every reconnect, so the caller need
 * not keep them alive.
 *
 * `unsubscribe_method` is the matching teardown call (e.g. "blockUnsubscribe"
 * for "blockSubscribe"), used by idx_pubsub_unsubscribe; NULL if the
 * subscription is never torn down individually.
 */
idx_status idx_pubsub_subscribe(idx_pubsub *pubsub, const char *method,
                                const char *unsubscribe_method,
                                const char *params_json, uint64_t *handle,
                                idx_error *err);

/*
 * Cancels a subscription. Sends the unsubscribe call when connected and
 * confirmed, and removes it from the registry so it is not re-established on
 * the next reconnect. Returns IDX_ERR_NOT_FOUND for an unknown handle.
 */
idx_status idx_pubsub_unsubscribe(idx_pubsub *pubsub, uint64_t handle,
                                  idx_error *err);

/*
 * Waits up to `timeout_ms` for one notification.
 *
 *   IDX_OK          `out` holds a message; free it with
 *                   idx_pubsub_message_free
 *   IDX_ERR_TIMEOUT nothing arrived within the budget. Reconnection may have
 *                   happened during the call; this is not an error
 *   IDX_ERR_REMOTE  the server rejected a subscription, which is a
 *                   configuration problem and will not fix itself
 *   anything else   the connection is unusable
 *
 * Subscription confirmations and control traffic are consumed internally and
 * never surface.
 */
idx_status idx_pubsub_poll(idx_pubsub *pubsub, int timeout_ms,
                           idx_pubsub_message *out, idx_error *err);

void idx_pubsub_message_free(idx_pubsub_message *message);

void idx_pubsub_get_stats(const idx_pubsub *pubsub, idx_pubsub_stats *out);

#endif /* IDX_PUBSUB_H */
