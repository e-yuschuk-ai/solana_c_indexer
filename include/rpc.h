/*
 * Solana JSON-RPC over HTTP (decision D1).
 *
 * This is the recovery path: the WebSocket carries the tip, and everything it
 * cannot replay — backfill, gaps after a reconnect, slots missed while the
 * socket was down — is fetched here.
 *
 * Compression is requested unconditionally. A block that is 12.2 MiB of JSON
 * arrives in 0.93 MiB gzipped, and libcurl decompresses it transparently, so
 * declining it would cost a factor of thirteen for nothing.
 *
 * A client owns one connection and is not thread-safe. The concurrent fetchers
 * in M4 each get their own, which is also what keeps connection reuse working.
 */
#ifndef IDX_RPC_H
#define IDX_RPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bytes.h"
#include "error.h"
#include "json.h"
#include "types.h"
#include "vec.h"

typedef struct idx_rpc idx_rpc;

typedef struct {
    /*
     * One or more endpoints. Calls start at the first and rotate on transport
     * failure, so ordering expresses preference.
     */
    const char *const *urls;
    size_t url_count;

    long connect_timeout_ms;
    long timeout_ms; /* whole request, including the transfer */

    int max_attempts; /* across all endpoints; 1 disables retrying */
    int backoff_initial_ms;
    int backoff_max_ms;

    /*
     * Largest slot span `getBlocks` may be asked for in one call. Solana's own
     * limit is 500000, but providers impose much smaller ones per plan — five
     * slots on some free tiers — so wider requests are split automatically.
     */
    uint64_t blocks_range_limit;

    bool verify_tls;
} idx_rpc_options;

/* Defaults: 10 s connect, 120 s total, 4 attempts, 250 ms to 8 s backoff. */
void idx_rpc_options_init(idx_rpc_options *options);

typedef struct {
    uint64_t requests;
    uint64_t retries;
    uint64_t rate_limited;  /* HTTP 429 responses */
    uint64_t failovers;     /* switches to another endpoint */
    uint64_t bytes_received;/* after decompression */
} idx_rpc_stats;

typedef struct {
    idx_json_doc *doc;   /* owned by the caller */
    idx_json_val result; /* the "result" member, borrowing `doc` */
} idx_rpc_response;

void idx_rpc_response_free(idx_rpc_response *response);

idx_status idx_rpc_open(const idx_rpc_options *options, idx_rpc **out,
                        idx_error *err);
void idx_rpc_close(idx_rpc *rpc);

/*
 * Calls `method` with `params_json` inserted verbatim (already valid JSON, or
 * NULL to omit).
 *
 *   IDX_OK            `response` holds the result
 *   IDX_ERR_NOT_FOUND the slot was skipped or is not retained by the endpoint.
 *                     This is a normal outcome, not a failure: Solana skips
 *                     slots routinely
 *   IDX_ERR_REMOTE    the endpoint rejected the request; retrying will not
 *                     help
 *   IDX_ERR_NETWORK   every attempt against every endpoint failed
 */
idx_status idx_rpc_call(idx_rpc *rpc, const char *method,
                        const char *params_json, idx_rpc_response *response,
                        idx_error *err);

/*
 * Sends several calls as one JSON-RPC batch, saving a round trip per call.
 * `responses` must have room for `count` entries; each is filled
 * independently, and entries whose call failed have a NULL `doc`.
 * `statuses`, when not NULL, receives the per-call status.
 *
 * The whole batch shares one document, so freeing any response frees them all;
 * use idx_rpc_batch_free rather than freeing them individually.
 */
typedef struct {
    const char *method;
    const char *params_json;
} idx_rpc_batch_call;

typedef struct {
    idx_json_doc *doc; /* shared by every entry */
    idx_json_val *results;
    idx_status *statuses;
    size_t count;
} idx_rpc_batch;

idx_status idx_rpc_call_batch(idx_rpc *rpc, const idx_rpc_batch_call *calls,
                              size_t count, idx_rpc_batch *batch,
                              idx_error *err);
void idx_rpc_batch_free(idx_rpc_batch *batch);

/* ---------------------------------------------------------------- methods -- */

/* `commitment` may be NULL for the endpoint's default. */
idx_status idx_rpc_get_slot(idx_rpc *rpc, const char *commitment,
                            uint64_t *out, idx_error *err);
idx_status idx_rpc_get_block_height(idx_rpc *rpc, const char *commitment,
                                    uint64_t *out, idx_error *err);

/* Returns IDX_OK when the node reports itself healthy. */
idx_status idx_rpc_get_health(idx_rpc *rpc, idx_error *err);

idx_status idx_rpc_get_version(idx_rpc *rpc, char *out, size_t out_size,
                               idx_error *err);

/*
 * Confirmed slots in [start, end]. Skipped slots are absent, which is what
 * makes this the way to enumerate a gap rather than guessing.
 * `slots` must be an idx_vec of uint64_t.
 */
idx_status idx_rpc_get_blocks(idx_rpc *rpc, uint64_t start, uint64_t end,
                              const char *commitment, idx_vec *slots,
                              idx_error *err);

typedef struct {
    const char *encoding;            /* "json" (default) or "base64" */
    const char *transaction_details; /* "full" (default), "accounts", ... */
    const char *commitment;          /* "confirmed" (default) or "finalized" */
    bool rewards;
    uint8_t max_supported_transaction_version;
} idx_rpc_block_options;

/* Defaults matching what the indexer ingests: json, full, confirmed, v0. */
void idx_rpc_block_options_init(idx_rpc_block_options *options);

idx_status idx_rpc_get_block(idx_rpc *rpc, uint64_t slot,
                             const idx_rpc_block_options *options,
                             idx_rpc_response *response, idx_error *err);

idx_status idx_rpc_get_transaction(idx_rpc *rpc, const idx_signature *signature,
                                   const idx_rpc_block_options *options,
                                   idx_rpc_response *response, idx_error *err);

void idx_rpc_get_stats(const idx_rpc *rpc, idx_rpc_stats *out);

/* The endpoint currently in use, for log messages. */
const char *idx_rpc_current_url(const idx_rpc *rpc);

#endif /* IDX_RPC_H */
