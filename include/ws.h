/*
 * WebSocket client over libcurl (decision D1).
 *
 * This layer knows about frames, not about JSON-RPC: it hands back complete
 * messages, having reassembled whatever fragmentation the peer used. The
 * measured `blockSubscribe` notifications are 4-8 MiB, so reassembly is the
 * normal case rather than an edge one, and the receive buffer is retained
 * between messages so the steady state performs no allocation.
 *
 * A connection is owned by one thread. Reconnection and resubscription belong
 * to the layer above; here, a dropped connection is reported as
 * IDX_ERR_CLOSED and the handle is finished.
 */
#ifndef IDX_WS_H
#define IDX_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bytes.h"
#include "error.h"

typedef struct idx_ws idx_ws;

typedef struct {
    const char *url; /* ws:// or wss:// */

    long connect_timeout_ms; /* 0 selects the default */

    /*
     * Upper bound on a single reassembled message. A peer that keeps sending
     * fragments must not be able to exhaust memory; exceeding this is a
     * protocol failure, not a resize.
     */
    size_t max_message_bytes;

    /* Capacity the receive buffer starts at, to avoid growing on every run. */
    size_t initial_buffer_bytes;

    bool verify_tls; /* leave true outside of local debugging */
} idx_ws_options;

/* Defaults: TLS verified, 16 MiB message cap, 8 MiB initial buffer. */
void idx_ws_options_init(idx_ws_options *options);

typedef struct {
    uint64_t messages_received;
    uint64_t bytes_received;
    uint64_t fragments_received;
    uint64_t pings_received;
    size_t largest_message; /* high-water mark, for sizing the buffer */
} idx_ws_stats;

/* Performs the TCP connect, the TLS handshake and the HTTP upgrade. */
idx_status idx_ws_connect(const idx_ws_options *options, idx_ws **out,
                          idx_error *err);

void idx_ws_close(idx_ws *ws);

/* Sends one complete text message. */
idx_status idx_ws_send_text(idx_ws *ws, idx_slice text, idx_error *err);

/*
 * Waits up to `timeout_ms` for one complete message.
 *
 *   IDX_OK          `message` borrows the connection's buffer and stays valid
 *                   until the next call to idx_ws_recv or idx_ws_close
 *   IDX_ERR_TIMEOUT nothing arrived; the connection is still usable
 *   IDX_ERR_CLOSED  the peer closed; the connection is finished
 *   anything else   fatal for this connection
 *
 * A partially received message is retained across an IDX_ERR_TIMEOUT, so a
 * caller polling with a short timeout still assembles large messages.
 */
idx_status idx_ws_recv(idx_ws *ws, int timeout_ms, idx_slice *message,
                       idx_error *err);

void idx_ws_get_stats(const idx_ws *ws, idx_ws_stats *out);

/* Bytes currently reserved by the receive buffer. */
size_t idx_ws_buffer_capacity(const idx_ws *ws);

#endif /* IDX_WS_H */
