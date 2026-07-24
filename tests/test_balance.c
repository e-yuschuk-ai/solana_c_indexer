/*
 * SOL balance extraction. The interesting assertions are about what is *not*
 * emitted — the accounts a transaction touched without moving — and about the
 * rows staying aligned with the account list once those are skipped.
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

TEST_MAIN({
    TEST_RUN(test_only_changed_accounts);
    TEST_RUN(test_nothing_moved);
    TEST_RUN(test_no_metadata);
    TEST_RUN(test_extremes);
    TEST_RUN(test_invalid_arguments);
})
