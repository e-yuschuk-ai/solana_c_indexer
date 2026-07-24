/*
 * Balance extraction, SOL and token. The interesting assertions are about what
 * is *not* emitted — the accounts a transaction touched without moving — and
 * about the rows staying aligned with the account list once those are skipped.
 * For token balances the join between the two sparse wire lists is the rest of
 * it: an account on one side only is a creation or a close, not a mismatch.
 */
#include "balance.h"

#include <string.h>

#include "test.h"

#define ACCOUNT_COUNT 5

typedef struct {
    idx_transaction tx;
    idx_account accounts[ACCOUNT_COUNT];
    uint64_t pre[ACCOUNT_COUNT];
    uint64_t post[ACCOUNT_COUNT];
    idx_token_balance pre_tokens[ACCOUNT_COUNT];
    idx_token_balance post_tokens[ACCOUNT_COUNT];
    idx_arena arena;
} fixture;

/* Account `i` is the key whose every byte is `i`. */
static void fixture_init(fixture *f) {
    memset(f, 0, sizeof(*f));
    for (size_t i = 0; i < ACCOUNT_COUNT; i++) {
        memset(f->accounts[i].pubkey.bytes, (int)i, IDX_PUBKEY_LEN);
    }
    f->tx.accounts = f->accounts;
    f->tx.account_count = ACCOUNT_COUNT;
    f->tx.pre_balances = f->pre;
    f->tx.post_balances = f->post;
    f->tx.balance_count = ACCOUNT_COUNT;
    idx_arena_init(&f->arena, 0);
}

static void fixture_free(fixture *f) {
    idx_arena_destroy(&f->arena);
}

static bool is_account(const idx_pubkey *key, uint8_t index) {
    idx_pubkey expected;
    memset(expected.bytes, index, IDX_PUBKEY_LEN);
    return idx_pubkey_equal(key, &expected);
}

static idx_status extract(fixture *f, const idx_sol_balance **out,
                          size_t *count) {
    idx_error err;
    idx_error_clear(&err);
    idx_status status =
        idx_sol_balance_extract(&f->tx, &f->arena, out, count, &err);
    if (status != IDX_OK) {
        TEST_CHECK(err.file != NULL, "failure recorded no context");
    }
    return status;
}

/* Mints and owners live in their own key ranges, so a row that picks up the
 * wrong field is visible rather than plausible. */
static bool is_mint(const idx_pubkey *key, uint8_t mint_id) {
    idx_pubkey expected;
    memset(expected.bytes, 0x80 + mint_id, IDX_PUBKEY_LEN);
    return idx_pubkey_equal(key, &expected);
}

static bool is_owner(const idx_pubkey *key, uint8_t account_index) {
    idx_pubkey expected;
    memset(expected.bytes, 0x40 + account_index, IDX_PUBKEY_LEN);
    return idx_pubkey_equal(key, &expected);
}

/* One pre/postTokenBalances entry: account `index` holding `amount` of mint
 * `mint_id`, owned by the wallet that goes with the account. */
static idx_token_balance holding(uint8_t index, uint8_t mint_id,
                                 uint64_t amount) {
    idx_token_balance entry;
    memset(&entry, 0, sizeof(entry));
    entry.account_index = index;
    memset(entry.mint.bytes, 0x80 + mint_id, IDX_PUBKEY_LEN);
    memset(entry.owner.bytes, 0x40 + index, IDX_PUBKEY_LEN);
    entry.has_owner = true;
    entry.amount = amount;
    entry.decimals = 6;
    return entry;
}

static void set_token_counts(fixture *f, size_t pre_count, size_t post_count) {
    f->tx.pre_token_balances = f->pre_tokens;
    f->tx.pre_token_balance_count = pre_count;
    f->tx.post_token_balances = f->post_tokens;
    f->tx.post_token_balance_count = post_count;
}

static idx_status extract_tokens(fixture *f,
                                 const idx_token_balance_state **out,
                                 size_t *count) {
    idx_error err;
    idx_error_clear(&err);
    idx_status status =
        idx_token_balance_extract(&f->tx, &f->arena, out, count, &err);
    if (status != IDX_OK) {
        TEST_CHECK(err.file != NULL, "failure recorded no context");
    }
    return status;
}

/* ---------------------------------------------------------------- tests -- */

static void test_only_changed_accounts(void) {
    fixture f;
    fixture_init(&f);

    /* A transfer of 4000 lamports from account 0 to account 3, with a 5000
     * lamport fee also charged to 0. Accounts 1, 2 and 4 are the program and
     * sysvars a transaction names without moving them. */
    f.pre[0] = 1000000000;
    f.post[0] = 1000000000 - 4000 - 5000;
    f.pre[1] = 1;
    f.post[1] = 1;
    f.pre[2] = 0;
    f.post[2] = 0;
    f.pre[3] = 0;
    f.post[3] = 4000;
    f.pre[4] = 953520;
    f.post[4] = 953520;

    const idx_sol_balance *balances = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &balances, &count), IDX_OK);
    TEST_EQ_UINT(count, 2);

    TEST_ASSERT(is_account(&balances[0].account, 0));
    TEST_EQ_UINT(balances[0].lamports, 999991000);
    TEST_EQ_INT(balances[0].delta, -9000);

    /* The rows close up: the second is account 3, not account 1. */
    TEST_ASSERT(is_account(&balances[1].account, 3));
    TEST_EQ_UINT(balances[1].lamports, 4000);
    TEST_EQ_INT(balances[1].delta, 4000);

    fixture_free(&f);
}

static void test_nothing_moved(void) {
    fixture f;
    fixture_init(&f);
    for (size_t i = 0; i < ACCOUNT_COUNT; i++) {
        f.pre[i] = 42;
        f.post[i] = 42;
    }

    const idx_sol_balance *balances = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &balances, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);
    TEST_ASSERT(balances == NULL);

    fixture_free(&f);
}

/* A block fetched without metadata carries no balances, which is not a
 * failure — there is simply nothing to extract. */
static void test_no_metadata(void) {
    fixture f;
    fixture_init(&f);
    f.tx.pre_balances = NULL;
    f.tx.post_balances = NULL;
    f.tx.balance_count = 0;

    const idx_sol_balance *balances = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &balances, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);
    TEST_ASSERT(balances == NULL);

    /* A count without the arrays is the same nothing, not a crash. */
    f.tx.balance_count = ACCOUNT_COUNT;
    TEST_EQ_INT(extract(&f, &balances, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);

    fixture_free(&f);
}

static void test_extremes(void) {
    fixture f;
    fixture_init(&f);
    for (size_t i = 0; i < ACCOUNT_COUNT; i++) {
        f.pre[i] = 0;
        f.post[i] = 0;
    }

    /* An account drained to nothing, and one funded from nothing. */
    f.pre[1] = 2039280;
    f.post[1] = 0;
    f.pre[2] = 0;
    f.post[2] = 2039280;

    const idx_sol_balance *balances = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &balances, &count), IDX_OK);
    TEST_EQ_UINT(count, 2);
    TEST_EQ_INT(balances[0].delta, -2039280);
    TEST_EQ_UINT(balances[0].lamports, 0);
    TEST_EQ_INT(balances[1].delta, 2039280);

    /* A movement no supply allows is a malformed block, not a balance. */
    f.pre[3] = 0;
    f.post[3] = UINT64_MAX;
    TEST_EQ_INT(extract(&f, &balances, &count), IDX_ERR_RANGE);

    fixture_free(&f);
}

static void test_invalid_arguments(void) {
    fixture f;
    fixture_init(&f);
    const idx_sol_balance *balances = NULL;
    size_t count = 0;

    TEST_EQ_INT(
        idx_sol_balance_extract(NULL, &f.arena, &balances, &count, NULL),
        IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_sol_balance_extract(&f.tx, NULL, &balances, &count, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_sol_balance_extract(&f.tx, &f.arena, NULL, &count, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_sol_balance_extract(&f.tx, &f.arena, &balances, NULL, NULL),
                IDX_ERR_INVALID_ARG);

    fixture_free(&f);
}

/* A transfer between two token accounts of the same mint, alongside a third
 * account the transaction named without trading against it. */
static void test_token_only_changed_accounts(void) {
    fixture f;
    fixture_init(&f);

    f.pre_tokens[0] = holding(1, 7, 100);
    f.pre_tokens[1] = holding(2, 7, 500);
    f.pre_tokens[2] = holding(3, 7, 9);
    f.post_tokens[0] = holding(1, 7, 60);
    f.post_tokens[1] = holding(2, 7, 540);
    f.post_tokens[2] = holding(3, 7, 9);
    set_token_counts(&f, 3, 3);

    const idx_token_balance_state *states = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract_tokens(&f, &states, &count), IDX_OK);
    TEST_EQ_UINT(count, 2);

    TEST_ASSERT(is_account(&states[0].account, 1));
    TEST_ASSERT(is_mint(&states[0].mint, 7));
    TEST_ASSERT(states[0].has_owner);
    TEST_ASSERT(is_owner(&states[0].owner, 1));
    TEST_EQ_UINT(states[0].amount, 60);
    TEST_EQ_UINT(states[0].previous, 100);
    TEST_EQ_UINT(states[0].decimals, 6);
    TEST_ASSERT(states[0].existed_before);
    TEST_ASSERT(!states[0].closed);

    /* The rows close up over the account that did not move, as they do for
     * SOL. */
    TEST_ASSERT(is_account(&states[1].account, 2));
    TEST_EQ_UINT(states[1].amount, 540);
    TEST_EQ_UINT(states[1].previous, 500);

    fixture_free(&f);
}

/* The two lists are sparse and independent: an account on one side only is a
 * token account that was created or closed, not a broken block. */
static void test_token_created_and_closed(void) {
    fixture f;
    fixture_init(&f);

    /* Account 1 is drained and closed, account 2 is created and funded from
     * it, account 3 is created and left empty, account 4 was already empty
     * when it was closed. */
    f.pre_tokens[0] = holding(1, 7, 250);
    f.pre_tokens[1] = holding(4, 7, 0);
    f.post_tokens[0] = holding(2, 7, 250);
    f.post_tokens[1] = holding(3, 7, 0);
    set_token_counts(&f, 2, 2);

    const idx_token_balance_state *states = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract_tokens(&f, &states, &count), IDX_OK);
    TEST_EQ_UINT(count, 2);

    TEST_ASSERT(is_account(&states[0].account, 2));
    TEST_EQ_UINT(states[0].amount, 250);
    TEST_EQ_UINT(states[0].previous, 0);
    TEST_ASSERT(!states[0].existed_before);
    TEST_ASSERT(!states[0].closed);

    TEST_ASSERT(is_account(&states[1].account, 1));
    TEST_ASSERT(is_mint(&states[1].mint, 7));
    TEST_EQ_UINT(states[1].amount, 0);
    TEST_EQ_UINT(states[1].previous, 250);
    TEST_ASSERT(states[1].existed_before);
    TEST_ASSERT(states[1].closed);

    fixture_free(&f);
}

/* Closing an account and recreating it for another mint reads as two holdings,
 * not as one balance jumping between tokens. */
static void test_token_mint_change(void) {
    fixture f;
    fixture_init(&f);

    f.pre_tokens[0] = holding(1, 7, 100);
    f.post_tokens[0] = holding(1, 8, 30);
    set_token_counts(&f, 1, 1);

    const idx_token_balance_state *states = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract_tokens(&f, &states, &count), IDX_OK);
    TEST_EQ_UINT(count, 2);

    TEST_ASSERT(is_mint(&states[0].mint, 8));
    TEST_EQ_UINT(states[0].amount, 30);
    TEST_EQ_UINT(states[0].previous, 0);
    TEST_ASSERT(!states[0].existed_before);

    TEST_ASSERT(is_mint(&states[1].mint, 7));
    TEST_EQ_UINT(states[1].amount, 0);
    TEST_EQ_UINT(states[1].previous, 100);
    TEST_ASSERT(states[1].closed);

    fixture_free(&f);
}

/* A raw token amount is bounded by the uint64 the mint's supply lives in, not
 * by anything an int64 holds, which is why the movement is a pair of amounts
 * rather than a signed delta. */
static void test_token_amounts_past_int64(void) {
    fixture f;
    fixture_init(&f);

    f.pre_tokens[0] = holding(1, 7, UINT64_MAX);
    f.post_tokens[0] = holding(1, 7, 0);
    f.post_tokens[1] = holding(2, 7, UINT64_MAX);
    set_token_counts(&f, 1, 2);

    const idx_token_balance_state *states = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract_tokens(&f, &states, &count), IDX_OK);
    TEST_EQ_UINT(count, 2);
    TEST_EQ_UINT(states[0].amount, 0);
    TEST_EQ_UINT(states[0].previous, UINT64_MAX);
    TEST_ASSERT(!states[0].closed); /* still a token account, just empty */
    TEST_EQ_UINT(states[1].amount, UINT64_MAX);
    TEST_EQ_UINT(states[1].previous, 0);

    fixture_free(&f);
}

/* Older blocks carry no owner on their token balances. */
static void test_token_without_owner(void) {
    fixture f;
    fixture_init(&f);

    f.pre_tokens[0] = holding(1, 7, 10);
    f.post_tokens[0] = holding(1, 7, 20);
    f.pre_tokens[0].has_owner = false;
    f.post_tokens[0].has_owner = false;
    set_token_counts(&f, 1, 1);

    const idx_token_balance_state *states = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract_tokens(&f, &states, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_ASSERT(!states[0].has_owner);
    TEST_EQ_UINT(states[0].amount, 20);

    fixture_free(&f);
}

/* One holding listed twice makes the join ambiguous, so it is rejected rather
 * than resolved by picking a side. */
static void test_token_duplicate_holding(void) {
    fixture f;
    fixture_init(&f);

    f.pre_tokens[0] = holding(1, 7, 10);
    f.pre_tokens[1] = holding(1, 7, 20);
    f.post_tokens[0] = holding(1, 7, 30);
    set_token_counts(&f, 2, 1);

    const idx_token_balance_state *states = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract_tokens(&f, &states, &count), IDX_ERR_PARSE);

    /* The same on the other side, and the same account holding two different
     * mints is not a duplicate at all. */
    f.pre_tokens[1] = holding(1, 8, 20);
    f.post_tokens[1] = holding(1, 8, 30);
    set_token_counts(&f, 2, 2);
    TEST_EQ_INT(extract_tokens(&f, &states, &count), IDX_OK);
    TEST_EQ_UINT(count, 2);

    f.post_tokens[1] = holding(1, 7, 40);
    TEST_EQ_INT(extract_tokens(&f, &states, &count), IDX_ERR_PARSE);

    fixture_free(&f);
}

static void test_token_no_metadata(void) {
    fixture f;
    fixture_init(&f);

    const idx_token_balance_state *states = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract_tokens(&f, &states, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);
    TEST_ASSERT(states == NULL);

    /* A transaction that touched no token account carries empty lists, and a
     * count without the arrays is the same nothing. */
    set_token_counts(&f, 0, 0);
    TEST_EQ_INT(extract_tokens(&f, &states, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);

    f.tx.pre_token_balances = NULL;
    f.tx.post_token_balances = NULL;
    f.tx.pre_token_balance_count = 2;
    f.tx.post_token_balance_count = 2;
    TEST_EQ_INT(extract_tokens(&f, &states, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);

    fixture_free(&f);
}

static void test_token_invalid_arguments(void) {
    fixture f;
    fixture_init(&f);
    const idx_token_balance_state *states = NULL;
    size_t count = 0;

    TEST_EQ_INT(
        idx_token_balance_extract(NULL, &f.arena, &states, &count, NULL),
        IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_token_balance_extract(&f.tx, NULL, &states, &count, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_token_balance_extract(&f.tx, &f.arena, NULL, &count, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_token_balance_extract(&f.tx, &f.arena, &states, NULL, NULL),
                IDX_ERR_INVALID_ARG);

    fixture_free(&f);
}

TEST_MAIN({
    TEST_RUN(test_only_changed_accounts);
    TEST_RUN(test_nothing_moved);
    TEST_RUN(test_no_metadata);
    TEST_RUN(test_extremes);
    TEST_RUN(test_invalid_arguments);
    TEST_RUN(test_token_only_changed_accounts);
    TEST_RUN(test_token_created_and_closed);
    TEST_RUN(test_token_mint_change);
    TEST_RUN(test_token_amounts_past_int64);
    TEST_RUN(test_token_without_owner);
    TEST_RUN(test_token_duplicate_holding);
    TEST_RUN(test_token_no_metadata);
    TEST_RUN(test_token_invalid_arguments);
})
