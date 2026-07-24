/*
 * Transfer extraction. The fixtures are hand-built transactions rather than
 * blocks: instruction bytes as the programs pack them, and the token balances
 * `meta` would carry alongside. What is asserted is the walk — top level then
 * the inner instructions each expanded into — and the enrichment that turns an
 * unchecked Transfer, which names neither mint nor scale, into a row that says
 * what moved.
 *
 * Account 0 is the System program, 1 the SPL Token program and 2 Token-2022,
 * so an instruction selects its program by index. Every other account `i` is
 * the key whose bytes are all `i`.
 */
#include "transfer.h"

#include <string.h>

#include "system_program.h"
#include "test.h"
#include "token_2022.h"
#include "token_program.h"

#define ACCOUNT_COUNT 12
#define MAX_INSTRUCTIONS 8
#define MAX_TOKEN_BALANCES 6

#define PROGRAM_SYSTEM 0
#define PROGRAM_TOKEN 1
#define PROGRAM_TOKEN_2022 2

typedef struct {
    uint8_t bytes[128];
    size_t len;
} payload;

typedef struct {
    idx_transaction tx;
    idx_account accounts[ACCOUNT_COUNT];
    idx_instruction instructions[MAX_INSTRUCTIONS];
    idx_instruction inner[MAX_INSTRUCTIONS];
    idx_inner_instructions inner_groups[MAX_INSTRUCTIONS];
    payload payloads[MAX_INSTRUCTIONS * 2];
    size_t payload_count;
    idx_token_balance pre_tokens[MAX_TOKEN_BALANCES];
    idx_token_balance post_tokens[MAX_TOKEN_BALANCES];
    idx_arena arena;
} fixture;

static void fixture_init(fixture *f) {
    memset(f, 0, sizeof(*f));
    for (size_t i = 0; i < ACCOUNT_COUNT; i++) {
        memset(f->accounts[i].pubkey.bytes, (int)i, IDX_PUBKEY_LEN);
        f->accounts[i].is_writable = true;
    }
    f->accounts[PROGRAM_SYSTEM].pubkey = IDX_PROGRAM_SYSTEM;
    f->accounts[PROGRAM_TOKEN].pubkey = IDX_PROGRAM_TOKEN;
    f->accounts[PROGRAM_TOKEN_2022].pubkey = IDX_PROGRAM_TOKEN_2022;

    f->tx.accounts = f->accounts;
    f->tx.account_count = ACCOUNT_COUNT;
    f->tx.instructions = f->instructions;
    f->tx.has_meta = true;
    f->tx.success = true;
    idx_arena_init(&f->arena, 0);
}

static void fixture_free(fixture *f) {
    idx_arena_destroy(&f->arena);
}

/* Every fixture key is one byte repeated: account `i`, and the mints and
 * wallets the token balances name. */
static bool is_key(const idx_pubkey *key, uint8_t fill) {
    idx_pubkey expected;
    memset(expected.bytes, fill, IDX_PUBKEY_LEN);
    return idx_pubkey_equal(key, &expected);
}

/* ------------------------------------------------------------- payloads -- */

static void put_u8(payload *p, uint8_t value) {
    p->bytes[p->len++] = value;
}

static void put_u32(payload *p, uint32_t value) {
    for (size_t i = 0; i < 4; i++) {
        put_u8(p, (uint8_t)((value >> (8 * i)) & 0xff));
    }
}

static void put_u64(payload *p, uint64_t value) {
    for (size_t i = 0; i < 8; i++) {
        put_u8(p, (uint8_t)((value >> (8 * i)) & 0xff));
    }
}

static void put_key(payload *p, uint8_t fill) {
    memset(p->bytes + p->len, fill, IDX_PUBKEY_LEN);
    p->len += IDX_PUBKEY_LEN;
}

static payload *next_payload(fixture *f) {
    payload *p = &f->payloads[f->payload_count++];
    p->len = 0;
    return p;
}

/*
 * Builds an instruction of `program` over `indices`, whose data the caller
 * fills through the returned payload. The account indices are borrowed, so they
 * must outlive the fixture use — the callers pass static arrays.
 */
static payload *set_instruction(fixture *f, idx_instruction *ix,
                                uint8_t program, const uint8_t *indices,
                                size_t index_count) {
    payload *p = next_payload(f);
    memset(ix, 0, sizeof(*ix));
    ix->program_id_index = program;
    ix->account_indices = indices;
    ix->account_count = index_count;
    ix->data = idx_slice_make(p->bytes, 0);
    return p;
}

/* Instruction data is a slice over the payload, so its length is only known
 * once the payload is filled. */
static void seal(idx_instruction *ix, const payload *p) {
    ix->data = idx_slice_make(p->bytes, p->len);
}

/* One token balance entry: account `index` holding `amount` of mint `mint_id`,
 * owned by the wallet whose key is `owner`. */
static idx_token_balance holding(uint8_t index, uint8_t mint_id, uint8_t owner,
                                 uint64_t amount) {
    idx_token_balance entry;
    memset(&entry, 0, sizeof(entry));
    entry.account_index = index;
    memset(entry.mint.bytes, mint_id, IDX_PUBKEY_LEN);
    memset(entry.owner.bytes, owner, IDX_PUBKEY_LEN);
    entry.has_owner = true;
    entry.amount = amount;
    entry.decimals = 9;
    return entry;
}

static idx_status extract(fixture *f, const idx_transfer **out, size_t *count) {
    idx_error err;
    idx_error_clear(&err);
    idx_status status = idx_transfer_extract(&f->tx, &f->arena, out, count, &err);
    if (status != IDX_OK) {
        TEST_CHECK(err.file != NULL, "failure recorded no context");
    }
    return status;
}

/* The account lists the fixture instructions draw from. */
static const uint8_t ACCOUNTS_A[] = {3, 4, 5, 6, 7};
static const uint8_t ACCOUNTS_B[] = {6, 7, 8, 9, 10};

/* ---------------------------------------------------------------- tests -- */

static void test_system_transfer(void) {
    fixture f;
    fixture_init(&f);

    payload *p = set_instruction(&f, &f.instructions[0], PROGRAM_SYSTEM,
                                 ACCOUNTS_A, 2);
    put_u32(p, IDX_SYSTEM_IX_TRANSFER);
    put_u64(p, 5000000);
    seal(&f.instructions[0], p);
    f.tx.instruction_count = 1;

    const idx_transfer *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].kind, IDX_TRANSFER_SOL);
    TEST_ASSERT(is_key(&rows[0].source, 3));
    TEST_ASSERT(is_key(&rows[0].destination, 4));
    TEST_EQ_UINT(rows[0].amount, 5000000);
    TEST_EQ_UINT(rows[0].instruction_index, 0);
    TEST_ASSERT(!rows[0].inner);
    TEST_ASSERT(!rows[0].has_mint);
    TEST_ASSERT(!rows[0].has_authority);

    fixture_free(&f);
}

/* Funding an account is a lamport movement like any other: the rent that
 * creates a token account leaves someone's wallet. */
static void test_system_create_account(void) {
    fixture f;
    fixture_init(&f);

    payload *p = set_instruction(&f, &f.instructions[0], PROGRAM_SYSTEM,
                                 ACCOUNTS_A, 2);
    put_u32(p, IDX_SYSTEM_IX_CREATE_ACCOUNT);
    put_u64(p, 2039280); /* lamports */
    put_u64(p, 165);     /* space */
    put_key(p, 0x11);    /* owner */
    seal(&f.instructions[0], p);

    /* An allocate in the same transaction moves nothing and produces no row. */
    payload *q = set_instruction(&f, &f.instructions[1], PROGRAM_SYSTEM,
                                 ACCOUNTS_A, 1);
    put_u32(q, IDX_SYSTEM_IX_ALLOCATE);
    put_u64(q, 165);
    seal(&f.instructions[1], q);
    f.tx.instruction_count = 2;

    const idx_transfer *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].kind, IDX_TRANSFER_SOL);
    TEST_ASSERT(is_key(&rows[0].source, 3));
    TEST_ASSERT(is_key(&rows[0].destination, 4));
    TEST_EQ_UINT(rows[0].amount, 2039280);

    fixture_free(&f);
}

/* An unchecked Transfer names neither the mint nor its scale. The token
 * balances in meta name both, and the owners the terminal indexes by. */
static void test_token_transfer_resolved_from_meta(void) {
    fixture f;
    fixture_init(&f);

    payload *p =
        set_instruction(&f, &f.instructions[0], PROGRAM_TOKEN, ACCOUNTS_A, 3);
    put_u8(p, IDX_TOKEN_IX_TRANSFER);
    put_u64(p, 1500);
    seal(&f.instructions[0], p);
    f.tx.instruction_count = 1;

    /* Accounts 3 and 4 hold mint 0x77 for wallets 0x21 and 0x22. */
    f.pre_tokens[0] = holding(3, 0x77, 0x21, 2000);
    f.pre_tokens[1] = holding(4, 0x77, 0x22, 0);
    f.post_tokens[0] = holding(3, 0x77, 0x21, 500);
    f.post_tokens[1] = holding(4, 0x77, 0x22, 1500);
    f.tx.pre_token_balances = f.pre_tokens;
    f.tx.pre_token_balance_count = 2;
    f.tx.post_token_balances = f.post_tokens;
    f.tx.post_token_balance_count = 2;

    const idx_transfer *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].kind, IDX_TRANSFER_TOKEN);
    TEST_ASSERT(is_key(&rows[0].source, 3));
    TEST_ASSERT(is_key(&rows[0].destination, 4));
    TEST_ASSERT(is_key(&rows[0].authority, 5));
    TEST_ASSERT(rows[0].has_authority);
    TEST_EQ_UINT(rows[0].amount, 1500);

    TEST_ASSERT(rows[0].has_mint);
    TEST_ASSERT(is_key(&rows[0].mint, 0x77));
    TEST_ASSERT(rows[0].has_decimals);
    TEST_EQ_UINT(rows[0].decimals, 9);
    TEST_ASSERT(rows[0].has_source_owner);
    TEST_ASSERT(is_key(&rows[0].source_owner, 0x21));
    TEST_ASSERT(rows[0].has_destination_owner);
    TEST_ASSERT(is_key(&rows[0].destination_owner, 0x22));

    /* Without meta's token balances the movement is still a row, it just
     * cannot say what moved. */
    f.tx.pre_token_balance_count = 0;
    f.tx.post_token_balance_count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_ASSERT(!rows[0].has_mint);
    TEST_ASSERT(!rows[0].has_source_owner);

    fixture_free(&f);
}

/* The checked form states the mint and the scale itself, and they win over the
 * balances. */
static void test_token_transfer_checked(void) {
    fixture f;
    fixture_init(&f);

    payload *p =
        set_instruction(&f, &f.instructions[0], PROGRAM_TOKEN, ACCOUNTS_A, 4);
    put_u8(p, IDX_TOKEN_IX_TRANSFER_CHECKED);
    put_u64(p, 42);
    put_u8(p, 6);
    seal(&f.instructions[0], p);
    f.tx.instruction_count = 1;

    const idx_transfer *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_ASSERT(is_key(&rows[0].source, 3));
    TEST_ASSERT(is_key(&rows[0].mint, 4));
    TEST_ASSERT(is_key(&rows[0].destination, 5));
    TEST_ASSERT(is_key(&rows[0].authority, 6));
    TEST_EQ_UINT(rows[0].decimals, 6);
    TEST_EQ_UINT(rows[0].amount, 42);

    fixture_free(&f);
}

/* Minted tokens come from the mint and burned ones go back to it, so both
 * read as a movement between two keys. */
static void test_token_mint_and_burn(void) {
    fixture f;
    fixture_init(&f);

    payload *p =
        set_instruction(&f, &f.instructions[0], PROGRAM_TOKEN, ACCOUNTS_A, 3);
    put_u8(p, IDX_TOKEN_IX_MINT_TO);
    put_u64(p, 900);
    seal(&f.instructions[0], p);

    payload *q =
        set_instruction(&f, &f.instructions[1], PROGRAM_TOKEN, ACCOUNTS_A, 3);
    put_u8(q, IDX_TOKEN_IX_BURN);
    put_u64(q, 100);
    seal(&f.instructions[1], q);
    f.tx.instruction_count = 2;

    /* Account 4 is the token account both touch; account 3 is the mint of the
     * MintTo and the burner's account of the Burn. */
    f.post_tokens[0] = holding(4, 0x77, 0x21, 900);
    f.tx.post_token_balances = f.post_tokens;
    f.tx.post_token_balance_count = 1;

    const idx_transfer *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 2);

    TEST_EQ_INT(rows[0].kind, IDX_TRANSFER_MINT);
    TEST_ASSERT(is_key(&rows[0].source, 3)); /* the mint */
    TEST_ASSERT(is_key(&rows[0].destination, 4));
    TEST_ASSERT(is_key(&rows[0].mint, 3));
    TEST_EQ_UINT(rows[0].amount, 900);
    /* Only the end that holds tokens is looked up, so the owner of the
     * destination is known and the mint has none. */
    TEST_ASSERT(rows[0].has_destination_owner);
    TEST_ASSERT(!rows[0].has_source_owner);
    TEST_EQ_UINT(rows[0].decimals, 9);

    TEST_EQ_INT(rows[1].kind, IDX_TRANSFER_BURN);
    TEST_ASSERT(is_key(&rows[1].source, 3));
    TEST_ASSERT(is_key(&rows[1].destination, 4)); /* the mint */
    TEST_ASSERT(is_key(&rows[1].mint, 4));
    TEST_EQ_UINT(rows[1].amount, 100);
    TEST_EQ_UINT(rows[1].instruction_index, 1);

    fixture_free(&f);
}

/* A mint that charges a transfer fee moves its tokens through the transfer fee
 * extension, not through TransferChecked. */
static void test_token_2022_transfer_with_fee(void) {
    fixture f;
    fixture_init(&f);

    payload *p = set_instruction(&f, &f.instructions[0], PROGRAM_TOKEN_2022,
                                 ACCOUNTS_A, 4);
    put_u8(p, IDX_TOKEN_2022_IX_TRANSFER_FEE_EXTENSION);
    put_u8(p, IDX_TOKEN_2022_FEE_IX_TRANSFER_CHECKED_WITH_FEE);
    put_u64(p, 10000); /* gross amount */
    put_u8(p, 6);      /* decimals */
    put_u64(p, 500);   /* fee withheld */
    seal(&f.instructions[0], p);

    /* A sibling sub-instruction of the same extension that moves nothing. */
    payload *q = set_instruction(&f, &f.instructions[1], PROGRAM_TOKEN_2022,
                                 ACCOUNTS_A, 2);
    put_u8(q, IDX_TOKEN_2022_IX_TRANSFER_FEE_EXTENSION);
    put_u8(q, IDX_TOKEN_2022_FEE_IX_HARVEST_TO_MINT);
    seal(&f.instructions[1], q);
    f.tx.instruction_count = 2;

    const idx_transfer *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].kind, IDX_TRANSFER_TOKEN);
    TEST_ASSERT(is_key(&rows[0].source, 3));
    TEST_ASSERT(is_key(&rows[0].mint, 4));
    TEST_ASSERT(is_key(&rows[0].destination, 5));
    TEST_ASSERT(is_key(&rows[0].authority, 6));
    TEST_EQ_UINT(rows[0].amount, 10000);
    TEST_EQ_UINT(rows[0].fee, 500);
    TEST_EQ_UINT(rows[0].decimals, 6);

    fixture_free(&f);
}

/* Token-2022's base instructions decode through the same path as SPL Token's,
 * so a transfer of either program produces the same row. */
static void test_token_2022_base_transfer(void) {
    fixture f;
    fixture_init(&f);

    payload *p = set_instruction(&f, &f.instructions[0], PROGRAM_TOKEN_2022,
                                 ACCOUNTS_A, 3);
    put_u8(p, IDX_TOKEN_IX_TRANSFER);
    put_u64(p, 77);
    seal(&f.instructions[0], p);
    f.tx.instruction_count = 1;

    const idx_transfer *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_INT(rows[0].kind, IDX_TRANSFER_TOKEN);
    TEST_EQ_UINT(rows[0].amount, 77);
    TEST_EQ_UINT(rows[0].fee, 0);

    fixture_free(&f);
}

/*
 * Most token movement is a CPI from a venue's program, so the inner
 * instructions are the ones that matter. They are walked after the top-level
 * instruction that made them, which is the order they ran in.
 */
static void test_inner_instructions(void) {
    fixture f;
    fixture_init(&f);

    /* A top-level instruction of an unknown program — a DEX — that transfers
     * twice through the token program. */
    payload *p = set_instruction(&f, &f.instructions[0], 11, ACCOUNTS_A, 5);
    put_u8(p, 9);
    seal(&f.instructions[0], p);

    payload *q =
        set_instruction(&f, &f.instructions[1], PROGRAM_SYSTEM, ACCOUNTS_A, 2);
    put_u32(q, IDX_SYSTEM_IX_TRANSFER);
    put_u64(q, 111);
    seal(&f.instructions[1], q);
    f.tx.instruction_count = 2;

    payload *a = set_instruction(&f, &f.inner[0], PROGRAM_TOKEN, ACCOUNTS_A, 3);
    put_u8(a, IDX_TOKEN_IX_TRANSFER);
    put_u64(a, 1000);
    seal(&f.inner[0], a);

    payload *b = set_instruction(&f, &f.inner[1], PROGRAM_TOKEN, ACCOUNTS_B, 3);
    put_u8(b, IDX_TOKEN_IX_TRANSFER);
    put_u64(b, 2000);
    seal(&f.inner[1], b);

    f.inner_groups[0].index = 0;
    f.inner_groups[0].instructions = f.inner;
    f.inner_groups[0].instruction_count = 2;
    f.tx.inner_instructions = f.inner_groups;
    f.tx.inner_instruction_count = 1;

    const idx_transfer *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 3);

    /* The two CPIs of instruction 0 come before instruction 1, because that is
     * when they ran. */
    TEST_EQ_UINT(rows[0].amount, 1000);
    TEST_ASSERT(rows[0].inner);
    TEST_EQ_UINT(rows[0].instruction_index, 0);
    TEST_EQ_UINT(rows[0].inner_index, 0);

    TEST_EQ_UINT(rows[1].amount, 2000);
    TEST_ASSERT(rows[1].inner);
    TEST_EQ_UINT(rows[1].inner_index, 1);
    TEST_ASSERT(is_key(&rows[1].source, 6));

    TEST_EQ_UINT(rows[2].amount, 111);
    TEST_ASSERT(!rows[2].inner);
    TEST_EQ_UINT(rows[2].instruction_index, 1);

    fixture_free(&f);
}

/* A failed transaction moved nothing: its instructions were rolled back, and a
 * row for one would claim something that did not happen. */
static void test_failed_transaction(void) {
    fixture f;
    fixture_init(&f);

    payload *p = set_instruction(&f, &f.instructions[0], PROGRAM_SYSTEM,
                                 ACCOUNTS_A, 2);
    put_u32(p, IDX_SYSTEM_IX_TRANSFER);
    put_u64(p, 5000000);
    seal(&f.instructions[0], p);
    f.tx.instruction_count = 1;

    const idx_transfer *rows = NULL;
    size_t count = 0;
    f.tx.success = false;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);
    TEST_ASSERT(rows == NULL);

    /* Without meta there is nothing that says whether it failed, so the same
     * rule applies. */
    f.tx.success = true;
    f.tx.has_meta = false;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);

    fixture_free(&f);
}

/* A transfer of nothing moves nothing, whatever program it went through. */
static void test_zero_amounts(void) {
    fixture f;
    fixture_init(&f);

    payload *p = set_instruction(&f, &f.instructions[0], PROGRAM_SYSTEM,
                                 ACCOUNTS_A, 2);
    put_u32(p, IDX_SYSTEM_IX_TRANSFER);
    put_u64(p, 0);
    seal(&f.instructions[0], p);

    payload *q =
        set_instruction(&f, &f.instructions[1], PROGRAM_TOKEN, ACCOUNTS_A, 3);
    put_u8(q, IDX_TOKEN_IX_TRANSFER);
    put_u64(q, 0);
    seal(&f.instructions[1], q);
    f.tx.instruction_count = 2;

    const idx_transfer *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);

    fixture_free(&f);
}

/*
 * A discriminant no decoder knows is a program upgrade and is skipped; a
 * truncated payload on an instruction the chain ran is this decoder being
 * wrong about the format, and is reported.
 */
static void test_unknown_and_truncated(void) {
    fixture f;
    fixture_init(&f);

    payload *p = set_instruction(&f, &f.instructions[0], PROGRAM_SYSTEM,
                                 ACCOUNTS_A, 2);
    put_u32(p, 99); /* no such System instruction */
    seal(&f.instructions[0], p);

    payload *q =
        set_instruction(&f, &f.instructions[1], PROGRAM_TOKEN, ACCOUNTS_A, 3);
    put_u8(q, IDX_TOKEN_IX_TRANSFER);
    put_u64(q, 5);
    seal(&f.instructions[1], q);
    f.tx.instruction_count = 2;

    const idx_transfer *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 1);
    TEST_EQ_UINT(rows[0].amount, 5);

    /* The amount of the transfer, one byte short. */
    f.payloads[1].len = 1 + 7;
    seal(&f.instructions[1], &f.payloads[1]);
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_ERR_RANGE);

    /* An instruction that names fewer accounts than the variant needs is the
     * same kind of disagreement. */
    f.payloads[1].len = 1 + 8;
    seal(&f.instructions[1], &f.payloads[1]);
    f.instructions[1].account_count = 2;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_ERR_PARSE);

    fixture_free(&f);
}

/* A transaction that touches neither program produces nothing. */
static void test_nothing_to_extract(void) {
    fixture f;
    fixture_init(&f);

    const idx_transfer *rows = NULL;
    size_t count = 0;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);
    TEST_ASSERT(rows == NULL);

    payload *p = set_instruction(&f, &f.instructions[0], 11, ACCOUNTS_A, 5);
    put_u8(p, 3);
    seal(&f.instructions[0], p);
    f.tx.instruction_count = 1;
    TEST_EQ_INT(extract(&f, &rows, &count), IDX_OK);
    TEST_EQ_UINT(count, 0);

    fixture_free(&f);
}

static void test_invalid_arguments(void) {
    fixture f;
    fixture_init(&f);
    const idx_transfer *rows = NULL;
    size_t count = 0;

    TEST_EQ_INT(idx_transfer_extract(NULL, &f.arena, &rows, &count, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_transfer_extract(&f.tx, NULL, &rows, &count, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_transfer_extract(&f.tx, &f.arena, NULL, &count, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_transfer_extract(&f.tx, &f.arena, &rows, NULL, NULL),
                IDX_ERR_INVALID_ARG);

    fixture_free(&f);
}

static void test_kind_names(void) {
    TEST_EQ_STR(idx_transfer_kind_name(IDX_TRANSFER_SOL), "sol");
    TEST_EQ_STR(idx_transfer_kind_name(IDX_TRANSFER_TOKEN), "token");
    TEST_EQ_STR(idx_transfer_kind_name(IDX_TRANSFER_MINT), "mint");
    TEST_EQ_STR(idx_transfer_kind_name(IDX_TRANSFER_BURN), "burn");
    TEST_EQ_STR(idx_transfer_kind_name((idx_transfer_kind)99), "unknown");
}

TEST_MAIN({
    TEST_RUN(test_system_transfer);
    TEST_RUN(test_system_create_account);
    TEST_RUN(test_token_transfer_resolved_from_meta);
    TEST_RUN(test_token_transfer_checked);
    TEST_RUN(test_token_mint_and_burn);
    TEST_RUN(test_token_2022_transfer_with_fee);
    TEST_RUN(test_token_2022_base_transfer);
    TEST_RUN(test_inner_instructions);
    TEST_RUN(test_failed_transaction);
    TEST_RUN(test_zero_amounts);
    TEST_RUN(test_unknown_and_truncated);
    TEST_RUN(test_nothing_to_extract);
    TEST_RUN(test_invalid_arguments);
    TEST_RUN(test_kind_names);
})
