#include "balance.h"

/*
 * The signed difference between two lamport balances. Both are bounded by the
 * total supply on a real chain, which is two orders of magnitude below what an
 * int64 holds, so the guard is against a malformed block rather than against
 * anything the chain can produce.
 */
static idx_status signed_delta(uint64_t before, uint64_t after, int64_t *out,
                               idx_error *err) {
    bool increased = after >= before;
    uint64_t magnitude = increased ? after - before : before - after;
    if (magnitude > (uint64_t)INT64_MAX) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "lamport balance moved by %llu, which no supply allows",
                        (unsigned long long)magnitude);
    }
    *out = increased ? (int64_t)magnitude : -(int64_t)magnitude;
    return IDX_OK;
}

idx_status idx_sol_balance_extract(const idx_transaction *tx, idx_arena *arena,
                                   const idx_sol_balance **out,
                                   size_t *out_count, idx_error *err) {
    if (tx == NULL || arena == NULL || out == NULL || out_count == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, arena, out and out_count must not be NULL");
    }

    *out = NULL;
    *out_count = 0;
    if (tx->balance_count == 0 || tx->pre_balances == NULL ||
        tx->post_balances == NULL) {
        return IDX_OK;
    }

    /* The count of accounts that moved is not known without walking them, and
     * the walk costs more than the arena space the unchanged ones waste. */
    void *raw = NULL;
    IDX_TRY(idx_arena_calloc(arena, tx->balance_count, sizeof(idx_sol_balance),
                             &raw, err));
    idx_sol_balance *balances = raw;

    size_t count = 0;
    for (size_t i = 0; i < tx->balance_count; i++) {
        uint64_t before = tx->pre_balances[i];
        uint64_t after = tx->post_balances[i];
        if (before == after) {
            continue;
        }
        idx_sol_balance *balance = &balances[count];
        IDX_TRY(signed_delta(before, after, &balance->delta, err));
        balance->account = tx->accounts[i].pubkey;
        balance->lamports = after;
        count++;
    }

    if (count == 0) {
        return IDX_OK;
    }
    *out = balances;
    *out_count = count;
    return IDX_OK;
}
