/*
 * Vote transaction filter. What matters here is not that a vote is caught —
 * that is one comparison — but that nothing else is: the cases below are the
 * shapes that would cost a transfer or a swap if the test were loosened to the
 * first instruction, or to any instruction, instead of all of them.
 */
#include "vote_filter.h"

#include <string.h>

#include "test.h"

#define ACCOUNT_COUNT 4

/* Account 0 is the Vote program, 1 the System program, 2 the SPL Token
 * program, 3 an ordinary account. */
typedef struct {
    idx_transaction tx;
    idx_account accounts[ACCOUNT_COUNT];
    idx_instruction instructions[4];
} fixture;

static void fixture_init(fixture *f) {
    memset(f, 0, sizeof(*f));
    f->accounts[0].pubkey = IDX_PROGRAM_VOTE;
    f->accounts[1].pubkey = IDX_PROGRAM_SYSTEM;
    f->accounts[2].pubkey = IDX_PROGRAM_TOKEN;
    memset(f->accounts[3].pubkey.bytes, 0x77, IDX_PUBKEY_LEN);
    f->tx.accounts = f->accounts;
    f->tx.account_count = ACCOUNT_COUNT;
    f->tx.instructions = f->instructions;
    f->tx.instruction_count = 0;
}

/* Appends an instruction invoking the program at `program_index`. */
static void push_ix(fixture *f, uint8_t program_index) {
    f->instructions[f->tx.instruction_count].program_id_index = program_index;
    f->tx.instruction_count++;
}

static void test_vote_is_dropped(void) {
    fixture f;
    fixture_init(&f);
    push_ix(&f, 0);
    TEST_ASSERT(idx_vote_filter_should_drop(&f.tx));

    /* More than one vote instruction is still nothing but votes. */
    push_ix(&f, 0);
    TEST_ASSERT(idx_vote_filter_should_drop(&f.tx));
}

static void test_non_vote_is_kept(void) {
    fixture f;
    fixture_init(&f);

    push_ix(&f, 1);
    TEST_ASSERT(!idx_vote_filter_should_drop(&f.tx));

    fixture_init(&f);
    push_ix(&f, 2);
    TEST_ASSERT(!idx_vote_filter_should_drop(&f.tx));

    /* A program that is nobody in particular. */
    fixture_init(&f);
    push_ix(&f, 3);
    TEST_ASSERT(!idx_vote_filter_should_drop(&f.tx));
}

/*
 * A transaction that mixes a vote with anything else is kept. Dropping it
 * would lose the other instruction, and there is no way back to it once the
 * block is gone.
 */
static void test_mixed_transaction_is_kept(void) {
    fixture f;

    fixture_init(&f);
    push_ix(&f, 0);
    push_ix(&f, 1);
    TEST_ASSERT(!idx_vote_filter_should_drop(&f.tx));

    /* The same, with the vote last, so the test cannot be passing by looking
     * only at the first instruction. */
    fixture_init(&f);
    push_ix(&f, 1);
    push_ix(&f, 0);
    TEST_ASSERT(!idx_vote_filter_should_drop(&f.tx));

    /* And with the vote in the middle of three. */
    fixture_init(&f);
    push_ix(&f, 2);
    push_ix(&f, 0);
    push_ix(&f, 1);
    TEST_ASSERT(!idx_vote_filter_should_drop(&f.tx));
}

static void test_edge_cases(void) {
    fixture f;
    fixture_init(&f);

    /* No instructions is not a vote. */
    TEST_ASSERT(!idx_vote_filter_should_drop(&f.tx));
    TEST_ASSERT(!idx_vote_filter_should_drop(NULL));
}

static void test_program_matches(void) {
    TEST_ASSERT(idx_vote_program_matches(&IDX_PROGRAM_VOTE));
    TEST_ASSERT(!idx_vote_program_matches(&IDX_PROGRAM_SYSTEM));
    TEST_ASSERT(!idx_vote_program_matches(&IDX_PROGRAM_TOKEN));
    TEST_ASSERT(!idx_vote_program_matches(NULL));

    /* The vote program is not the default pubkey, which the system program
     * is; a zeroed program id must not read as a vote. */
    TEST_ASSERT(!idx_vote_program_matches(&IDX_PUBKEY_DEFAULT));
}

TEST_MAIN({
    TEST_RUN(test_vote_is_dropped);
    TEST_RUN(test_non_vote_is_kept);
    TEST_RUN(test_mixed_transaction_is_kept);
    TEST_RUN(test_edge_cases);
    TEST_RUN(test_program_matches);
})
