#include "rpc.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

#define IDX_RPC_DEFAULT_CONNECT_TIMEOUT_MS 10000
#define IDX_RPC_DEFAULT_TIMEOUT_MS 120000
#define IDX_RPC_DEFAULT_MAX_ATTEMPTS 4
#define IDX_RPC_DEFAULT_BACKOFF_INITIAL_MS 250
#define IDX_RPC_DEFAULT_BACKOFF_MAX_MS 8000

/* Solana's own ceiling for getBlocks. Providers narrow it per plan. */
#define IDX_RPC_DEFAULT_BLOCKS_RANGE 500000

/*
 * Solana reports a slot that was skipped, or that the endpoint no longer
 * retains, as a JSON-RPC error rather than an empty result. Skipped slots are
 * routine, so these are translated into IDX_ERR_NOT_FOUND and never retried:
 * the block is not coming.
 */
#define IDX_RPC_ERR_SLOT_SKIPPED (-32007)
#define IDX_RPC_ERR_SLOT_NOT_AVAILABLE (-32004)
#define IDX_RPC_ERR_LONG_TERM_STORAGE (-32009)

struct idx_rpc {
    CURL *curl;
    struct curl_slist *headers;

    char **urls;
    size_t url_count;
    size_t current_url;

    idx_rpc_options options;
    idx_buffer response; /* reused across calls */
    idx_buffer request;
    uint64_t next_id;
    idx_rpc_stats stats;
    uint32_t rng_state;
};

void idx_rpc_options_init(idx_rpc_options *options) {
    if (options == NULL) {
        return;
    }
    options->urls = NULL;
    options->url_count = 0;
    options->connect_timeout_ms = IDX_RPC_DEFAULT_CONNECT_TIMEOUT_MS;
    options->timeout_ms = IDX_RPC_DEFAULT_TIMEOUT_MS;
    options->max_attempts = IDX_RPC_DEFAULT_MAX_ATTEMPTS;
    options->backoff_initial_ms = IDX_RPC_DEFAULT_BACKOFF_INITIAL_MS;
    options->backoff_max_ms = IDX_RPC_DEFAULT_BACKOFF_MAX_MS;
    options->blocks_range_limit = IDX_RPC_DEFAULT_BLOCKS_RANGE;
    options->verify_tls = true;
}

void idx_rpc_block_options_init(idx_rpc_block_options *options) {
    if (options == NULL) {
        return;
    }
    options->encoding = "json";
    options->transaction_details = "full";
    options->commitment = "confirmed";
    options->rewards = false;
    options->max_supported_transaction_version = 0;
}

static uint32_t next_random(idx_rpc *rpc) {
    uint32_t x = rpc->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rpc->rng_state = x;
    return x;
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int milliseconds) {
    if (milliseconds <= 0) {
        return;
    }
    struct timespec request;
    request.tv_sec = milliseconds / 1000;
    request.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&request, NULL);
}

static size_t on_data(char *data, size_t size, size_t count, void *user_data) {
    idx_rpc *rpc = user_data;
    size_t total = size * count;
    if (idx_buffer_append(&rpc->response, data, total, NULL) != IDX_OK) {
        return 0; /* aborts the transfer */
    }
    return total;
}

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

idx_status idx_rpc_open(const idx_rpc_options *options, idx_rpc **out,
                        idx_error *err) {
    if (options == NULL || out == NULL || options->urls == NULL ||
        options->url_count == 0) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "at least one endpoint URL is required");
    }
    IDX_TRY(ensure_curl_global(err));

    idx_rpc *rpc = calloc(1, sizeof(*rpc));
    if (rpc == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to allocate the RPC client");
    }

    rpc->options = *options;
    rpc->next_id = 1;
    rpc->rng_state = (uint32_t)monotonic_ms() | 1u;
    idx_buffer_init(&rpc->response);
    idx_buffer_init(&rpc->request);

    if (rpc->options.max_attempts < 1) {
        rpc->options.max_attempts = 1;
    }
    if (rpc->options.backoff_initial_ms <= 0) {
        rpc->options.backoff_initial_ms = IDX_RPC_DEFAULT_BACKOFF_INITIAL_MS;
    }
    if (rpc->options.backoff_max_ms < rpc->options.backoff_initial_ms) {
        rpc->options.backoff_max_ms = rpc->options.backoff_initial_ms;
    }

    /* URLs are copied so the caller's strings need not outlive the client. */
    rpc->urls = calloc(options->url_count, sizeof(*rpc->urls));
    if (rpc->urls == NULL) {
        free(rpc);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "failed to copy the endpoints");
    }
    for (size_t i = 0; i < options->url_count; i++) {
        rpc->urls[i] = strdup(options->urls[i]);
        if (rpc->urls[i] == NULL) {
            for (size_t j = 0; j < i; j++) {
                free(rpc->urls[j]);
            }
            free(rpc->urls);
            free(rpc);
            return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                            "failed to copy endpoint %zu", i);
        }
        rpc->url_count++;
    }
    rpc->options.urls = NULL; /* the copies are authoritative */

    rpc->curl = curl_easy_init();
    if (rpc->curl == NULL) {
        idx_rpc_close(rpc);
        return IDX_FAIL(err, IDX_ERR_INTERNAL, "curl_easy_init failed");
    }

    rpc->headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (rpc->headers == NULL) {
        idx_rpc_close(rpc);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "failed to build the headers");
    }

    curl_easy_setopt(rpc->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(rpc->curl, CURLOPT_HTTPHEADER, rpc->headers);
    curl_easy_setopt(rpc->curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(rpc->curl, CURLOPT_WRITEDATA, rpc);
    curl_easy_setopt(rpc->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(rpc->curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(rpc->curl, CURLOPT_USERAGENT, "solana_c_indexer/0.1");
    curl_easy_setopt(rpc->curl, CURLOPT_CONNECTTIMEOUT_MS,
                     rpc->options.connect_timeout_ms);
    curl_easy_setopt(rpc->curl, CURLOPT_TIMEOUT_MS, rpc->options.timeout_ms);
    curl_easy_setopt(rpc->curl, CURLOPT_SSL_VERIFYPEER,
                     rpc->options.verify_tls ? 1L : 0L);
    curl_easy_setopt(rpc->curl, CURLOPT_SSL_VERIFYHOST,
                     rpc->options.verify_tls ? 2L : 0L);
    /* An empty string means "every encoding this build supports". Blocks
     * compress about thirteen to one, so this is the single most valuable
     * option here. */
    curl_easy_setopt(rpc->curl, CURLOPT_ACCEPT_ENCODING, "");

    *out = rpc;
    return IDX_OK;
}

void idx_rpc_close(idx_rpc *rpc) {
    if (rpc == NULL) {
        return;
    }
    if (rpc->curl != NULL) {
        curl_easy_cleanup(rpc->curl);
    }
    if (rpc->headers != NULL) {
        curl_slist_free_all(rpc->headers);
    }
    for (size_t i = 0; i < rpc->url_count; i++) {
        free(rpc->urls[i]);
    }
    free(rpc->urls);
    idx_buffer_free(&rpc->response);
    idx_buffer_free(&rpc->request);
    free(rpc);
}

const char *idx_rpc_current_url(const idx_rpc *rpc) {
    if (rpc == NULL || rpc->url_count == 0) {
        return "";
    }
    return rpc->urls[rpc->current_url];
}

static void rotate_endpoint(idx_rpc *rpc) {
    if (rpc->url_count < 2) {
        return;
    }
    rpc->current_url = (rpc->current_url + 1) % rpc->url_count;
    rpc->stats.failovers++;
    IDX_WARN("failing over to %s", rpc->urls[rpc->current_url]);
}

static int backoff_for(idx_rpc *rpc, int attempt) {
    int64_t base = rpc->options.backoff_initial_ms;
    for (int i = 0; i < attempt && base < rpc->options.backoff_max_ms; i++) {
        base *= 2;
    }
    if (base > rpc->options.backoff_max_ms) {
        base = rpc->options.backoff_max_ms;
    }
    /* Full jitter: sleep somewhere in [base/2, base]. */
    int half = (int)(base / 2);
    return half + (int)(next_random(rpc) % (uint32_t)(half + 1));
}

/* Performs one HTTP round trip, leaving the body in rpc->response. */
static idx_status perform(idx_rpc *rpc, idx_slice body, long *http_status,
                          idx_error *err) {
    idx_buffer_clear(&rpc->response);

    curl_easy_setopt(rpc->curl, CURLOPT_URL, rpc->urls[rpc->current_url]);
    curl_easy_setopt(rpc->curl, CURLOPT_POSTFIELDS, (const char *)body.data);
    curl_easy_setopt(rpc->curl, CURLOPT_POSTFIELDSIZE, (long)body.len);

    CURLcode code = curl_easy_perform(rpc->curl);
    if (code != CURLE_OK) {
        return IDX_FAIL(err, IDX_ERR_NETWORK, "%s: %s",
                        rpc->urls[rpc->current_url], curl_easy_strerror(code));
    }

    curl_easy_getinfo(rpc->curl, CURLINFO_RESPONSE_CODE, http_status);
    rpc->stats.bytes_received += rpc->response.len;
    return IDX_OK;
}

/* Translates a JSON-RPC error object into a status. */
static idx_status classify_rpc_error(idx_json_val error_object,
                                     const char *method, idx_error *err) {
    int64_t code = 0;
    idx_slice text = idx_slice_from_str("(no message)");
    idx_json_read_i64(error_object, "code", &code, NULL);
    idx_json_opt_string(error_object, "message", &text);

    if (code == IDX_RPC_ERR_SLOT_SKIPPED ||
        code == IDX_RPC_ERR_SLOT_NOT_AVAILABLE ||
        code == IDX_RPC_ERR_LONG_TERM_STORAGE) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "%s: %.*s (code %lld)", method,
                        (int)text.len, (const char *)text.data,
                        (long long)code);
    }
    return IDX_FAIL(err, IDX_ERR_REMOTE, "%s: %.*s (code %lld)", method,
                    (int)text.len, (const char *)text.data, (long long)code);
}

/*
 * Decides whether an HTTP status is worth another attempt. 429 and 5xx are
 * transient; other 4xx mean the request itself is wrong.
 */
static bool http_status_is_retryable(long status) {
    return status == 429 || (status >= 500 && status < 600);
}

/*
 * Providers often answer a rejected request with a non-2xx status *and* a
 * JSON-RPC error object explaining it — a plan limit, say. The status alone
 * ("HTTP 413") tells the operator nothing, so the body is preferred whenever
 * it parses.
 */
static void describe_http_failure(idx_rpc *rpc, const char *what,
                                  long http_status, idx_error *err) {
    idx_json_doc *doc = NULL;
    if (idx_json_parse(idx_buffer_slice(&rpc->response), &doc, NULL) == IDX_OK) {
        idx_json_val error_object = idx_json_get(idx_json_root(doc), "error");
        idx_slice text;
        if (idx_json_is_present(error_object) &&
            idx_json_opt_string(error_object, "message", &text)) {
            int64_t code = 0;
            idx_json_read_i64(error_object, "code", &code, NULL);
            IDX_FAIL(err, IDX_ERR_REMOTE, "%s: %.*s (code %lld, HTTP %ld)",
                     what, (int)text.len, (const char *)text.data,
                     (long long)code, http_status);
            idx_json_free(doc);
            return;
        }
        idx_json_free(doc);
    }
    IDX_FAIL(err, IDX_ERR_REMOTE, "%s: HTTP %ld from %s", what, http_status,
             rpc->urls[rpc->current_url]);
}

/* Runs the request, retrying transport failures and transient statuses. */
static idx_status send_with_retries(idx_rpc *rpc, idx_slice body,
                                    const char *what, idx_error *err) {
    idx_error attempt_err;
    idx_error_clear(&attempt_err);

    for (int attempt = 0; attempt < rpc->options.max_attempts; attempt++) {
        if (attempt > 0) {
            int delay = backoff_for(rpc, attempt - 1);
            IDX_DEBUG("retrying %s in %d ms (attempt %d/%d)", what, delay,
                      attempt + 1, rpc->options.max_attempts);
            sleep_ms(delay);
            rpc->stats.retries++;
        }

        long http_status = 0;
        idx_status status = perform(rpc, body, &http_status, &attempt_err);

        if (status == IDX_OK && http_status == 200) {
            rpc->stats.requests++;
            return IDX_OK;
        }

        if (status == IDX_OK) {
            if (http_status == 429) {
                rpc->stats.rate_limited++;
                IDX_WARN("%s was rate limited by %s", what,
                         rpc->urls[rpc->current_url]);
            }
            if (!http_status_is_retryable(http_status)) {
                describe_http_failure(rpc, what, http_status, err);
                return IDX_ERR_REMOTE;
            }
            IDX_FAIL(&attempt_err, IDX_ERR_NETWORK, "HTTP %ld", http_status);
        }

        /* Both a transport failure and a retryable status are reasons to try
         * somewhere else, if there is a somewhere else. */
        rotate_endpoint(rpc);
    }

    return IDX_FAIL(err, IDX_ERR_NETWORK, "%s failed after %d attempts: %s",
                    what, rpc->options.max_attempts, attempt_err.message);
}

void idx_rpc_response_free(idx_rpc_response *response) {
    if (response == NULL) {
        return;
    }
    idx_json_free(response->doc);
    memset(response, 0, sizeof(*response));
}

idx_status idx_rpc_call(idx_rpc *rpc, const char *method,
                        const char *params_json, idx_rpc_response *response,
                        idx_error *err) {
    if (rpc == NULL || method == NULL || response == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "rpc, method and response must not be NULL");
    }
    memset(response, 0, sizeof(*response));

    idx_buffer_clear(&rpc->request);
    IDX_TRY(idx_json_write_rpc_request(&rpc->request, rpc->next_id++, method,
                                       params_json, err));
    /* POSTFIELDS needs a NUL-terminated string; the length is set separately
     * so the terminator is not part of the body. */
    IDX_TRY(idx_buffer_append_byte(&rpc->request, '\0', err));

    idx_slice body =
        idx_slice_make(rpc->request.data, rpc->request.len - 1);
    IDX_TRY(send_with_retries(rpc, body, method, err));

    idx_json_doc *doc = NULL;
    IDX_TRY(idx_json_parse(idx_buffer_slice(&rpc->response), &doc, err));

    idx_json_val root = idx_json_root(doc);
    idx_json_val error_object = idx_json_get(root, "error");
    if (idx_json_is_present(error_object) && !idx_json_is_null(error_object)) {
        idx_status status = classify_rpc_error(error_object, method, err);
        idx_json_free(doc);
        return status;
    }

    idx_json_val result = idx_json_get(root, "result");
    if (!idx_json_is_present(result)) {
        idx_json_free(doc);
        return IDX_FAIL(err, IDX_ERR_PARSE, "%s: response has no result",
                        method);
    }

    response->doc = doc;
    response->result = result;
    return IDX_OK;
}

/* ---------------------------------------------------------------- batching -- */

void idx_rpc_batch_free(idx_rpc_batch *batch) {
    if (batch == NULL) {
        return;
    }
    idx_json_free(batch->doc);
    free(batch->results);
    free(batch->statuses);
    memset(batch, 0, sizeof(*batch));
}

idx_status idx_rpc_call_batch(idx_rpc *rpc, const idx_rpc_batch_call *calls,
                              size_t count, idx_rpc_batch *batch,
                              idx_error *err) {
    if (rpc == NULL || calls == NULL || batch == NULL || count == 0) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "rpc, calls and batch must not be NULL");
    }
    memset(batch, 0, sizeof(*batch));

    idx_buffer_clear(&rpc->request);
    IDX_TRY(idx_buffer_append_byte(&rpc->request, '[', err));

    /*
     * Responses may come back in any order, so the id assigned here is what
     * maps each one to its call.
     */
    uint64_t first_id = rpc->next_id;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            IDX_TRY(idx_buffer_append_byte(&rpc->request, ',', err));
        }
        IDX_TRY(idx_json_write_rpc_request(&rpc->request, rpc->next_id++,
                                           calls[i].method,
                                           calls[i].params_json, err));
    }
    IDX_TRY(idx_buffer_append_byte(&rpc->request, ']', err));
    IDX_TRY(idx_buffer_append_byte(&rpc->request, '\0', err));

    idx_slice body = idx_slice_make(rpc->request.data, rpc->request.len - 1);
    IDX_TRY(send_with_retries(rpc, body, "batch", err));

    idx_json_doc *doc = NULL;
    IDX_TRY(idx_json_parse(idx_buffer_slice(&rpc->response), &doc, err));

    idx_json_val root = idx_json_root(doc);
    if (!idx_json_is_array(root)) {
        idx_json_free(doc);
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "batch response is %s, expected an array",
                        idx_json_type_name(root));
    }

    batch->results = calloc(count, sizeof(*batch->results));
    batch->statuses = calloc(count, sizeof(*batch->statuses));
    if (batch->results == NULL || batch->statuses == NULL) {
        idx_json_free(doc);
        free(batch->results);
        free(batch->statuses);
        memset(batch, 0, sizeof(*batch));
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY,
                        "failed to allocate %zu batch entries", count);
    }
    batch->doc = doc;
    batch->count = count;

    for (size_t i = 0; i < count; i++) {
        batch->statuses[i] = IDX_ERR_NOT_FOUND;
    }

    size_t cursor = 0;
    idx_json_val entry;
    while (idx_json_array_next(root, &cursor, &entry)) {
        uint64_t id = 0;
        if (!idx_json_opt_u64(entry, "id", &id) || id < first_id ||
            id - first_id >= count) {
            IDX_WARN("batch response carries an unexpected id");
            continue;
        }
        size_t index = (size_t)(id - first_id);

        idx_json_val error_object = idx_json_get(entry, "error");
        if (idx_json_is_present(error_object) &&
            !idx_json_is_null(error_object)) {
            batch->statuses[index] =
                classify_rpc_error(error_object, calls[index].method, NULL);
            continue;
        }

        idx_json_val result = idx_json_get(entry, "result");
        if (!idx_json_is_present(result)) {
            batch->statuses[index] = IDX_ERR_PARSE;
            continue;
        }
        batch->results[index] = result;
        batch->statuses[index] = IDX_OK;
    }

    return IDX_OK;
}

/* ----------------------------------------------------------------- methods -- */

/* Builds `[{"commitment":"..."}]`, or `[]` when none was given. */
static void commitment_params(const char *commitment, char *out,
                              size_t out_size) {
    if (commitment == NULL) {
        snprintf(out, out_size, "[]");
    } else {
        snprintf(out, out_size, "[{\"commitment\":\"%s\"}]", commitment);
    }
}

static idx_status call_for_u64(idx_rpc *rpc, const char *method,
                               const char *params, uint64_t *out,
                               idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    idx_rpc_response response;
    IDX_TRY(idx_rpc_call(rpc, method, params, &response, err));

    idx_status status = idx_json_as_u64(response.result, method, out, err);
    idx_rpc_response_free(&response);
    return status;
}

idx_status idx_rpc_get_slot(idx_rpc *rpc, const char *commitment,
                            uint64_t *out, idx_error *err) {
    char params[128];
    commitment_params(commitment, params, sizeof(params));
    return call_for_u64(rpc, "getSlot", params, out, err);
}

idx_status idx_rpc_get_block_height(idx_rpc *rpc, const char *commitment,
                                    uint64_t *out, idx_error *err) {
    char params[128];
    commitment_params(commitment, params, sizeof(params));
    return call_for_u64(rpc, "getBlockHeight", params, out, err);
}

idx_status idx_rpc_get_health(idx_rpc *rpc, idx_error *err) {
    idx_rpc_response response;
    IDX_TRY(idx_rpc_call(rpc, "getHealth", "[]", &response, err));

    idx_slice text;
    idx_status status = idx_json_as_string(response.result, "getHealth", &text,
                                           err);
    if (status == IDX_OK && !idx_slice_equal(text, idx_slice_from_str("ok"))) {
        status = IDX_FAIL(err, IDX_ERR_REMOTE, "node reports '%.*s'",
                          (int)text.len, (const char *)text.data);
    }
    idx_rpc_response_free(&response);
    return status;
}

idx_status idx_rpc_get_version(idx_rpc *rpc, char *out, size_t out_size,
                               idx_error *err) {
    if (out == NULL || out_size == 0) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "out must not be NULL");
    }
    idx_rpc_response response;
    IDX_TRY(idx_rpc_call(rpc, "getVersion", "[]", &response, err));

    idx_slice version;
    idx_status status =
        idx_json_read_string(response.result, "solana-core", &version, err);
    if (status == IDX_OK) {
        size_t length = (version.len < out_size - 1) ? version.len
                                                     : out_size - 1;
        memcpy(out, version.data, length);
        out[length] = '\0';
    }
    idx_rpc_response_free(&response);
    return status;
}

/* One getBlocks call, within whatever range the provider accepts. */
static idx_status get_blocks_chunk(idx_rpc *rpc, uint64_t start, uint64_t end,
                                   const char *commitment, idx_vec *slots,
                                   idx_error *err) {
    char params[192];
    if (commitment != NULL) {
        snprintf(params, sizeof(params), "[%llu,%llu,{\"commitment\":\"%s\"}]",
                 (unsigned long long)start, (unsigned long long)end,
                 commitment);
    } else {
        snprintf(params, sizeof(params), "[%llu,%llu]",
                 (unsigned long long)start, (unsigned long long)end);
    }

    idx_rpc_response response;
    IDX_TRY(idx_rpc_call(rpc, "getBlocks", params, &response, err));

    idx_status status = IDX_OK;
    if (!idx_json_is_array(response.result)) {
        status = IDX_FAIL(err, IDX_ERR_PARSE, "getBlocks returned %s",
                          idx_json_type_name(response.result));
    } else {
        size_t cursor = 0;
        idx_json_val entry;
        while (status == IDX_OK &&
               idx_json_array_next(response.result, &cursor, &entry)) {
            uint64_t slot = 0;
            status = idx_json_as_u64(entry, "slot", &slot, err);
            if (status == IDX_OK) {
                status = idx_vec_push(slots, &slot, err);
            }
        }
    }

    idx_rpc_response_free(&response);
    return status;
}

idx_status idx_rpc_get_blocks(idx_rpc *rpc, uint64_t start, uint64_t end,
                              const char *commitment, idx_vec *slots,
                              idx_error *err) {
    if (rpc == NULL || slots == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "rpc and slots must not be NULL");
    }
    if (end < start) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "end slot %llu is before start slot %llu",
                        (unsigned long long)end, (unsigned long long)start);
    }

    uint64_t limit = rpc->options.blocks_range_limit;
    if (limit == 0) {
        limit = IDX_RPC_DEFAULT_BLOCKS_RANGE;
    }

    /*
     * The range is walked in chunks the provider will accept, so callers can
     * ask for a gap of any width without knowing the plan's limit.
     */
    uint64_t from = start;
    while (from <= end) {
        uint64_t span = end - from;
        uint64_t to = (span >= limit) ? from + limit - 1 : end;
        IDX_TRY(get_blocks_chunk(rpc, from, to, commitment, slots, err));
        if (to == end) {
            break;
        }
        from = to + 1;
    }
    return IDX_OK;
}

/* Serializes the block options common to getBlock and getTransaction. */
static void block_config(const idx_rpc_block_options *options, char *out,
                         size_t out_size) {
    idx_rpc_block_options defaults;
    if (options == NULL) {
        idx_rpc_block_options_init(&defaults);
        options = &defaults;
    }
    snprintf(out, out_size,
             "{\"encoding\":\"%s\",\"transactionDetails\":\"%s\","
             "\"commitment\":\"%s\",\"rewards\":%s,"
             "\"maxSupportedTransactionVersion\":%u}",
             options->encoding, options->transaction_details,
             options->commitment, options->rewards ? "true" : "false",
             (unsigned)options->max_supported_transaction_version);
}

idx_status idx_rpc_get_block(idx_rpc *rpc, uint64_t slot,
                             const idx_rpc_block_options *options,
                             idx_rpc_response *response, idx_error *err) {
    char config[256];
    block_config(options, config, sizeof(config));

    char params[320];
    snprintf(params, sizeof(params), "[%llu,%s]", (unsigned long long)slot,
             config);

    return idx_rpc_call(rpc, "getBlock", params, response, err);
}

idx_status idx_rpc_get_transaction(idx_rpc *rpc, const idx_signature *signature,
                                   const idx_rpc_block_options *options,
                                   idx_rpc_response *response, idx_error *err) {
    if (signature == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "signature must not be NULL");
    }

    char text[IDX_SIGNATURE_STR_MAX];
    IDX_TRY(idx_signature_to_base58(signature, text, err));

    idx_rpc_block_options defaults;
    if (options == NULL) {
        idx_rpc_block_options_init(&defaults);
        options = &defaults;
    }

    /* getTransaction accepts neither transactionDetails nor rewards, so its
     * config is built separately rather than reusing block_config. */
    char params[416];
    snprintf(params, sizeof(params),
             "[\"%s\",{\"encoding\":\"%s\",\"commitment\":\"%s\","
             "\"maxSupportedTransactionVersion\":%u}]",
             text, options->encoding, options->commitment,
             (unsigned)options->max_supported_transaction_version);

    return idx_rpc_call(rpc, "getTransaction", params, response, err);
}

void idx_rpc_get_stats(const idx_rpc *rpc, idx_rpc_stats *out) {
    if (rpc == NULL || out == NULL) {
        return;
    }
    *out = rpc->stats;
}
