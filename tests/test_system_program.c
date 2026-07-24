/*
 * System program instruction decoding. The fixtures are built byte by byte in
 * the bincode layout the runtime uses, rather than through the block decoder,
 * so a change to either format fails here on its own terms. Account operands
 * are checked by identity: account `i` of the fixture transaction is the key
 * whose every byte is `i`.
 */
#include "system_program.h"

#include <string.h>

#include "test.h"

#define ACCOUNT_COUNT 8

typedef struct {
    idx_transaction tx;
    idx_account accounts[ACCOUNT_COUNT];
} fixture;

static void fixture_init(fixture *f) {
    memset(f, 0, sizeof(*f));
    for (size_t i = 0; i < ACCOUNT_COUNT; i++) {
        memset(f->accounts[i].pubkey.bytes, (int)i, IDX_PUBKEY_LEN);
        f->accounts[i].is_writable = true;
    }
    f->tx.accounts = f->accounts;
    f->tx.account_count = ACCOUNT_COUNT;
}

/* True when `key` is the fixture account at `index`. */
static bool is_account(const idx_pubkey *key, uint8_t index) {
    if (key == NULL) {
        return false;
    }
    idx_pubkey expected;
    memset(expected.bytes, index, IDX_PUBKEY_LEN);
    return idx_pubkey_equal(key, &expected);
}

static bool is_filled_key(const idx_pubkey *key, uint8_t fill) {
    idx_pubkey expected;
    memset(expected.bytes, fill, IDX_PUBKEY_LEN);
    return idx_pubkey_equal(key, &expected);
}

/* ------------------------------------------------------------- payloads -- */

typedef struct {
    uint8_t bytes[256];
    size_t len;
} payload;

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

/* A pubkey field: 32 bytes all equal to `fill`. */
static void put_key(payload *p, uint8_t fill) {
    memset(p->bytes + p->len, fill, IDX_PUBKEY_LEN);
    p->len += IDX_PUBKEY_LEN;
}

/* A bincode String: a u64 length, then the bytes. */
static void put_seed(payload *p, const char *seed) {
    size_t len = strlen(seed);
    put_u64(p, (uint64_t)len);
    memcpy(p->bytes + p->len, seed, len);
    p->len += len;
}

static idx_instruction make_ix(const uint8_t *indices, size_t index_count,
                               const payload *data) {
    idx_instruction ix;
    memset(&ix, 0, sizeof(ix));
    ix.account_indices = indices;
    ix.account_count = index_count;
    ix.data = idx_slice_make(data->bytes, data->len);
    return ix;
}

/* Decodes and reports the failure message when it was not expected. */
static idx_status decode(const fixture *f, const idx_instruction *ix,
                         idx_system_instruction *out) {
    idx_error err;
    idx_error_clear(&err);
    idx_status status = idx_system_instruction_decode(&f->tx, ix, out, &err);
    if (status != IDX_OK) {
        TEST_CHECK(err.file != NULL, "failure recorded no context");
    }
    return status;
}

/* ---------------------------------------------------------------- tests -- */

static void test_transfer(void) {
    fixture f;
    fixture_init(&f);

    payload data = {{0}, 0};
    put_u32(&data, IDX_SYSTEM_IX_TRANSFER);
    put_u64(&data, 5000);

    const uint8_t indices[] = {3, 5};
    idx_instruction ix = make_ix(indices, 2, &data);
    idx_system_instruction decoded;
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_INT(decoded.kind, IDX_SYSTEM_IX_TRANSFER);
    TEST_ASSERT(is_account(decoded.transfer.from, 3));
    TEST_ASSERT(is_account(decoded.transfer.to, 5));
    TEST_EQ_UINT(decoded.transfer.lamports, 5000);
}

/* The runtime ignores bytes past the fields it reads, and so does this. */
static void test_transfer_trailing_bytes(void) {
    fixture f;
    fixture_init(&f);

    payload data = {{0}, 0};
    put_u32(&data, IDX_SYSTEM_IX_TRANSFER);
    put_u64(&data, 1);
    put_u64(&data, 0xdeadbeef);

    const uint8_t indices[] = {0, 1};
    idx_instruction ix = make_ix(indices, 2, &data);
    idx_system_instruction decoded;
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_UINT(decoded.transfer.lamports, 1);
}

static void test_create_account(void) {
    fixture f;
    fixture_init(&f);

    payload data = {{0}, 0};
    put_u32(&data, IDX_SYSTEM_IX_CREATE_ACCOUNT);
    put_u64(&data, 2039280);
    put_u64(&data, 165);
    put_key(&data, 0xaa);

    const uint8_t indices[] = {0, 1};
    idx_instruction ix = make_ix(indices, 2, &data);
    idx_system_instruction decoded;
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_INT(decoded.kind, IDX_SYSTEM_IX_CREATE_ACCOUNT);
    TEST_ASSERT(is_account(decoded.create_account.funder, 0));
    TEST_ASSERT(is_account(decoded.create_account.account, 1));
    TEST_EQ_UINT(decoded.create_account.lamports, 2039280);
    TEST_EQ_UINT(decoded.create_account.space, 165);
    TEST_ASSERT(is_filled_key(&decoded.create_account.owner, 0xaa));
}

static void test_create_account_with_seed(void) {
    fixture f;
    fixture_init(&f);

    payload data = {{0}, 0};
    put_u32(&data, IDX_SYSTEM_IX_CREATE_ACCOUNT_WITH_SEED);
    put_key(&data, 0xbb);
    put_seed(&data, "stake:0");
    put_u64(&data, 1000000);
    put_u64(&data, 200);
    put_key(&data, 0xcc);

    const uint8_t indices[] = {2, 4, 2};
    idx_instruction ix = make_ix(indices, 3, &data);
    idx_system_instruction decoded;
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.create_account_with_seed.funder, 2));
    TEST_ASSERT(is_account(decoded.create_account_with_seed.account, 4));
    TEST_ASSERT(is_filled_key(&decoded.create_account_with_seed.base, 0xbb));
    TEST_ASSERT(idx_slice_equal(decoded.create_account_with_seed.seed,
                                idx_slice_from_str("stake:0")));
    TEST_EQ_UINT(decoded.create_account_with_seed.lamports, 1000000);
    TEST_EQ_UINT(decoded.create_account_with_seed.space, 200);
    TEST_ASSERT(is_filled_key(&decoded.create_account_with_seed.owner, 0xcc));
}

static void test_transfer_with_seed(void) {
    fixture f;
    fixture_init(&f);

    payload data = {{0}, 0};
    put_u32(&data, IDX_SYSTEM_IX_TRANSFER_WITH_SEED);
    put_u64(&data, 42);
    put_seed(&data, "seed");
    put_key(&data, 0xdd);

    const uint8_t indices[] = {1, 2, 3};
    idx_instruction ix = make_ix(indices, 3, &data);
    idx_system_instruction decoded;
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.transfer_with_seed.from, 1));
    TEST_ASSERT(is_account(decoded.transfer_with_seed.base_account, 2));
    TEST_ASSERT(is_account(decoded.transfer_with_seed.to, 3));
    TEST_EQ_UINT(decoded.transfer_with_seed.lamports, 42);
    TEST_ASSERT(idx_slice_equal(decoded.transfer_with_seed.from_seed,
                                idx_slice_from_str("seed")));
    TEST_ASSERT(is_filled_key(&decoded.transfer_with_seed.from_owner, 0xdd));
}

static void test_assign_and_allocate(void) {
    fixture f;
    fixture_init(&f);
    idx_system_instruction decoded;

    payload assign = {{0}, 0};
    put_u32(&assign, IDX_SYSTEM_IX_ASSIGN);
    put_key(&assign, 0x11);
    const uint8_t one[] = {6};
    idx_instruction ix = make_ix(one, 1, &assign);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_INT(decoded.kind, IDX_SYSTEM_IX_ASSIGN);
    TEST_ASSERT(is_account(decoded.assign.account, 6));
    TEST_ASSERT(is_filled_key(&decoded.assign.owner, 0x11));

    payload allocate = {{0}, 0};
    put_u32(&allocate, IDX_SYSTEM_IX_ALLOCATE);
    put_u64(&allocate, 82);
    ix = make_ix(one, 1, &allocate);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_INT(decoded.kind, IDX_SYSTEM_IX_ALLOCATE);
    TEST_ASSERT(is_account(decoded.allocate.account, 6));
    TEST_EQ_UINT(decoded.allocate.space, 82);

    payload allocate_seed = {{0}, 0};
    put_u32(&allocate_seed, IDX_SYSTEM_IX_ALLOCATE_WITH_SEED);
    put_key(&allocate_seed, 0x22);
    put_seed(&allocate_seed, "a");
    put_u64(&allocate_seed, 1);
    put_key(&allocate_seed, 0x33);
    const uint8_t two[] = {6, 7};
    ix = make_ix(two, 2, &allocate_seed);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.allocate_with_seed.account, 6));
    TEST_ASSERT(is_account(decoded.allocate_with_seed.base_account, 7));
    TEST_ASSERT(is_filled_key(&decoded.allocate_with_seed.base, 0x22));
    TEST_EQ_UINT(decoded.allocate_with_seed.space, 1);
    TEST_ASSERT(is_filled_key(&decoded.allocate_with_seed.owner, 0x33));

    payload assign_seed = {{0}, 0};
    put_u32(&assign_seed, IDX_SYSTEM_IX_ASSIGN_WITH_SEED);
    put_key(&assign_seed, 0x44);
    put_seed(&assign_seed, "bc");
    put_key(&assign_seed, 0x55);
    ix = make_ix(two, 2, &assign_seed);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.assign_with_seed.account, 6));
    TEST_ASSERT(is_account(decoded.assign_with_seed.base_account, 7));
    TEST_ASSERT(idx_slice_equal(decoded.assign_with_seed.seed,
                                idx_slice_from_str("bc")));
    TEST_ASSERT(is_filled_key(&decoded.assign_with_seed.owner, 0x55));
}

/* The sysvar accounts a nonce instruction carries sit between the operands
 * that matter, so the positions are what this checks. */
static void test_nonce_instructions(void) {
    fixture f;
    fixture_init(&f);
    idx_system_instruction decoded;

    payload advance = {{0}, 0};
    put_u32(&advance, IDX_SYSTEM_IX_ADVANCE_NONCE_ACCOUNT);
    const uint8_t three[] = {1, 2, 3};
    idx_instruction ix = make_ix(three, 3, &advance);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.nonce.nonce, 1));
    TEST_ASSERT(is_account(decoded.nonce.authority, 3));

    payload upgrade = {{0}, 0};
    put_u32(&upgrade, IDX_SYSTEM_IX_UPGRADE_NONCE_ACCOUNT);
    ix = make_ix(three, 1, &upgrade);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.nonce.nonce, 1));
    TEST_ASSERT(decoded.nonce.authority == NULL);

    payload withdraw = {{0}, 0};
    put_u32(&withdraw, IDX_SYSTEM_IX_WITHDRAW_NONCE_ACCOUNT);
    put_u64(&withdraw, 777);
    const uint8_t five[] = {1, 2, 3, 4, 5};
    ix = make_ix(five, 5, &withdraw);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.withdraw_nonce.nonce, 1));
    TEST_ASSERT(is_account(decoded.withdraw_nonce.to, 2));
    TEST_ASSERT(is_account(decoded.withdraw_nonce.authority, 5));
    TEST_EQ_UINT(decoded.withdraw_nonce.lamports, 777);

    payload initialize = {{0}, 0};
    put_u32(&initialize, IDX_SYSTEM_IX_INITIALIZE_NONCE_ACCOUNT);
    put_key(&initialize, 0x66);
    ix = make_ix(three, 3, &initialize);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.set_nonce_authority.nonce, 1));
    TEST_ASSERT(decoded.set_nonce_authority.authority == NULL);
    TEST_ASSERT(
        is_filled_key(&decoded.set_nonce_authority.new_authority, 0x66));

    payload authorize = {{0}, 0};
    put_u32(&authorize, IDX_SYSTEM_IX_AUTHORIZE_NONCE_ACCOUNT);
    put_key(&authorize, 0x77);
    ix = make_ix(three, 2, &authorize);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.set_nonce_authority.nonce, 1));
    TEST_ASSERT(is_account(decoded.set_nonce_authority.authority, 2));
    TEST_ASSERT(
        is_filled_key(&decoded.set_nonce_authority.new_authority, 0x77));
}

static void test_errors(void) {
    fixture f;
    fixture_init(&f);
    idx_system_instruction decoded;
    const uint8_t indices[] = {0, 1};

    /* A discriminant past the last variant is a program this decoder has not
     * caught up with, not corruption. */
    payload unknown = {{0}, 0};
    put_u32(&unknown, IDX_SYSTEM_IX_UPGRADE_NONCE_ACCOUNT + 1);
    idx_instruction ix = make_ix(indices, 2, &unknown);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_NOT_FOUND);

    payload empty = {{0}, 0};
    ix = make_ix(indices, 2, &empty);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_RANGE);

    payload truncated = {{0}, 0};
    put_u32(&truncated, IDX_SYSTEM_IX_TRANSFER);
    put_u8(&truncated, 1);
    ix = make_ix(indices, 2, &truncated);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_RANGE);

    payload transfer = {{0}, 0};
    put_u32(&transfer, IDX_SYSTEM_IX_TRANSFER);
    put_u64(&transfer, 1);
    ix = make_ix(indices, 1, &transfer);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);
    ix = make_ix(indices, 0, &transfer);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);

    /* A seed longer than the data that follows it. */
    payload bad_seed = {{0}, 0};
    put_u32(&bad_seed, IDX_SYSTEM_IX_ASSIGN_WITH_SEED);
    put_key(&bad_seed, 0x01);
    put_u64(&bad_seed, UINT64_MAX);
    ix = make_ix(indices, 2, &bad_seed);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_RANGE);

    /* An advance nonce that names the nonce but not the authority. */
    payload advance = {{0}, 0};
    put_u32(&advance, IDX_SYSTEM_IX_ADVANCE_NONCE_ACCOUNT);
    ix = make_ix(indices, 2, &advance);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);

    TEST_EQ_INT(idx_system_instruction_decode(NULL, &ix, &decoded, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_system_instruction_decode(&f.tx, NULL, &decoded, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_system_instruction_decode(&f.tx, &ix, NULL, NULL),
                IDX_ERR_INVALID_ARG);
}

static void test_kind_names(void) {
    TEST_EQ_STR(idx_system_ix_kind_name(IDX_SYSTEM_IX_TRANSFER), "Transfer");
    TEST_EQ_STR(idx_system_ix_kind_name(IDX_SYSTEM_IX_TRANSFER_WITH_SEED),
                "TransferWithSeed");
    TEST_EQ_STR(idx_system_ix_kind_name((idx_system_ix_kind)99), "unknown");
}

TEST_MAIN({
    TEST_RUN(test_transfer);
    TEST_RUN(test_transfer_trailing_bytes);
    TEST_RUN(test_create_account);
    TEST_RUN(test_create_account_with_seed);
    TEST_RUN(test_transfer_with_seed);
    TEST_RUN(test_assign_and_allocate);
    TEST_RUN(test_nonce_instructions);
    TEST_RUN(test_errors);
    TEST_RUN(test_kind_names);
})
