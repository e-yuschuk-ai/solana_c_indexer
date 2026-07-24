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

/* Whether two wire entries describe the same token account holding the same
 * mint, which is what the two sides are joined on. */
static bool same_holding(const idx_token_balance *a,
                         const idx_token_balance *b) {
    return a->account_index == b->account_index &&
           idx_pubkey_equal(&a->mint, &b->mint);
}

/*
 * Rejects a list that names one holding twice. The join would otherwise pair
 * the same account with two different amounts and emit both, leaving M7 to
 * upsert contradictory state in whatever order it read them.
 */
static idx_status check_unique(const idx_token_balance *list, size_t count,
                               const char *side, idx_error *err) {
    for (size_t i = 1; i < count; i++) {
        for (size_t j = 0; j < i; j++) {
            if (same_holding(&list[i], &list[j])) {
                return IDX_FAIL(err, IDX_ERR_PARSE,
                                "%s lists account index %u twice for the same "
                                "mint",
                                side, (unsigned)list[i].account_index);
            }
        }
    }
    return IDX_OK;
}

/* The entry for `needle`'s holding in `list`, or NULL. The lists hold a handful
 * of entries each, so the scan costs less than any index over them would. */
static const idx_token_balance *find_holding(const idx_token_balance *list,
                                             size_t count,
                                             const idx_token_balance *needle) {
    for (size_t i = 0; i < count; i++) {
        if (same_holding(&list[i], needle)) {
            return &list[i];
        }
    }
    return NULL;
}

/* Fills the fields both sides of the join share, from whichever entry is the
 * more recent view of the account. */
static void fill_holding(idx_token_balance_state *state,
                         const idx_transaction *tx,
                         const idx_token_balance *entry) {
    state->account = tx->accounts[entry->account_index].pubkey;
    state->mint = entry->mint;
    state->decimals = entry->decimals;
    state->has_owner = entry->has_owner;
    if (entry->has_owner) {
        state->owner = entry->owner;
    }
}

idx_status idx_token_balance_extract(const idx_transaction *tx, idx_arena *arena,
                                     const idx_token_balance_state **out,
                                     size_t *out_count, idx_error *err) {
    if (tx == NULL || arena == NULL || out == NULL || out_count == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, arena, out and out_count must not be NULL");
    }

    *out = NULL;
    *out_count = 0;
    const idx_token_balance *pre = tx->pre_token_balances;
    const idx_token_balance *post = tx->post_token_balances;
    size_t pre_count = (pre != NULL) ? tx->pre_token_balance_count : 0;
    size_t post_count = (post != NULL) ? tx->post_token_balance_count : 0;
    if ((pre_count == 0 && post_count == 0) || tx->accounts == NULL) {
        return IDX_OK;
    }

    IDX_TRY(check_unique(pre, pre_count, "preTokenBalances", err));
    IDX_TRY(check_unique(post, post_count, "postTokenBalances", err));

    /* Nothing joins in the worst case, so this is the bound: every holding on
     * either side its own row. */
    void *raw = NULL;
    IDX_TRY(idx_arena_calloc(arena, pre_count + post_count,
                             sizeof(idx_token_balance_state), &raw, err));
    idx_token_balance_state *states = raw;

    size_t count = 0;
    for (size_t i = 0; i < post_count; i++) {
        const idx_token_balance *after = &post[i];
        const idx_token_balance *before = find_holding(pre, pre_count, after);
        uint64_t previous = (before != NULL) ? before->amount : 0;
        if (previous == after->amount) {
            /* Also the account created and left empty: an account holding
             * nothing has nothing for a terminal to show, whether it existed a
             * moment ago or not. */
            continue;
        }

        idx_token_balance_state *state = &states[count++];
        /* The post entry is the current view: an owner or a mint authority
         * changed mid-transaction is reported as it ended up. */
        fill_holding(state, tx, after);
        state->amount = after->amount;
        state->existed_before = before != NULL;
        state->previous = previous;
    }

    /* What the post list does not mention no longer holds the mint. */
    for (size_t i = 0; i < pre_count; i++) {
        const idx_token_balance *before = &pre[i];
        if (before->amount == 0 ||
            find_holding(post, post_count, before) != NULL) {
            continue;
        }

        idx_token_balance_state *state = &states[count++];
        fill_holding(state, tx, before);
        state->amount = 0;
        state->previous = before->amount;
        state->existed_before = true;
        state->closed = true;
    }

    if (count == 0) {
        return IDX_OK;
    }
    *out = states;
    *out_count = count;
    return IDX_OK;
}
