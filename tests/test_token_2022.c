/*
 * Token-2022 instruction decoding. The fixtures are packed bytes, as in
 * test_token_program.c, and account operands are checked by identity: account
 * `i` of the fixture transaction is the key whose every byte is `i`. What is
 * asserted here is the split — a shared instruction reaching `base`, a
 * Token-2022 one reaching its own variant, and an extension group identified
 * without its payload being interpreted.
 */
#include "token_2022.h"

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
    uint8_t bytes[128];
    size_t len;
} payload;

static void put_u8(payload *p, uint8_t value) {
    p->bytes[p->len++] = value;
}

static void put_u16(payload *p, uint16_t value) {
    put_u8(p, (uint8_t)(value & 0xff));
    put_u8(p, (uint8_t)((value >> 8) & 0xff));
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

static idx_instruction make_ix(const uint8_t *indices, size_t index_count,
                               const payload *data) {
    idx_instruction ix;
    memset(&ix, 0, sizeof(ix));
    ix.account_indices = indices;
    ix.account_count = index_count;
    ix.data = idx_slice_make(data->bytes, data->len);
    return ix;
}

static idx_status decode(const fixture *f, const idx_instruction *ix,
                         idx_token_2022_instruction *out) {
    idx_error err;
    idx_error_clear(&err);
    idx_status status =
        idx_token_2022_instruction_decode(&f->tx, ix, out, &err);
    if (status != IDX_OK) {
        TEST_CHECK(err.file != NULL, "failure recorded no context");
    }
    return status;
}

static const uint8_t INDICES[] = {1, 2, 3, 4, 5};

/* ---------------------------------------------------------------- tests -- */

/* The shared range still decodes, and says so. */
static void test_base_instructions_pass_through(void) {
    fixture f;
    fixture_init(&f);
    idx_token_2022_instruction decoded;

    payload transfer = {{0}, 0};
    put_u8(&transfer, IDX_TOKEN_IX_TRANSFER_CHECKED);
    put_u64(&transfer, 4200);
    put_u8(&transfer, 9);
    idx_instruction ix = make_ix(INDICES, 4, &transfer);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(decoded.is_base);
    TEST_EQ_INT(decoded.base.kind, IDX_TOKEN_IX_TRANSFER_CHECKED);
    TEST_ASSERT(is_account(decoded.base.transfer.source, 1));
    TEST_ASSERT(is_account(decoded.base.transfer.mint, 2));
    TEST_EQ_UINT(decoded.base.transfer.amount, 4200);
    TEST_EQ_UINT(decoded.base.transfer.decimals, 9);

    /* A failure inside the shared range is that instruction's failure, not a
     * reason to try the Token-2022 range. */
    payload short_accounts = {{0}, 0};
    put_u8(&short_accounts, IDX_TOKEN_IX_TRANSFER);
    put_u64(&short_accounts, 1);
    ix = make_ix(INDICES, 2, &short_accounts);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);
}

static void test_initialize_mint_close_authority(void) {
    fixture f;
    fixture_init(&f);
    idx_token_2022_instruction decoded;

    payload data = {{0}, 0};
    put_u8(&data, IDX_TOKEN_2022_IX_INITIALIZE_MINT_CLOSE_AUTHORITY);
    put_u8(&data, 1);
    put_key(&data, 0xc1);
    idx_instruction ix = make_ix(INDICES, 1, &data);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(!decoded.is_base);
    TEST_EQ_INT(decoded.kind,
                IDX_TOKEN_2022_IX_INITIALIZE_MINT_CLOSE_AUTHORITY);
    TEST_ASSERT(is_account(decoded.initialize_mint_close_authority.mint, 1));
    TEST_ASSERT(decoded.initialize_mint_close_authority.has_close_authority);
    TEST_ASSERT(is_filled_key(
        &decoded.initialize_mint_close_authority.close_authority, 0xc1));

    /* No close authority means the mint can never be closed. */
    payload none = {{0}, 0};
    put_u8(&none, IDX_TOKEN_2022_IX_INITIALIZE_MINT_CLOSE_AUTHORITY);
    put_u8(&none, 0);
    ix = make_ix(INDICES, 1, &none);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(!decoded.initialize_mint_close_authority.has_close_authority);

    payload bad_tag = {{0}, 0};
    put_u8(&bad_tag, IDX_TOKEN_2022_IX_INITIALIZE_MINT_CLOSE_AUTHORITY);
    put_u8(&bad_tag, 7);
    ix = make_ix(INDICES, 1, &bad_tag);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);
}

static void test_mint_variants(void) {
    fixture f;
    fixture_init(&f);
    idx_token_2022_instruction decoded;

    payload delegate = {{0}, 0};
    put_u8(&delegate, IDX_TOKEN_2022_IX_INITIALIZE_PERMANENT_DELEGATE);
    put_key(&delegate, 0xc2);
    idx_instruction ix = make_ix(INDICES, 1, &delegate);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.initialize_permanent_delegate.mint, 1));
    TEST_ASSERT(
        is_filled_key(&decoded.initialize_permanent_delegate.delegate, 0xc2));

    payload non_transferable = {{0}, 0};
    put_u8(&non_transferable,
           IDX_TOKEN_2022_IX_INITIALIZE_NON_TRANSFERABLE_MINT);
    ix = make_ix(INDICES, 1, &non_transferable);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.mint_only.mint, 1));
    TEST_ASSERT(decoded.mint_only.payer == NULL);

    /* CreateNativeMint puts the payer first, the mint second. */
    payload native = {{0}, 0};
    put_u8(&native, IDX_TOKEN_2022_IX_CREATE_NATIVE_MINT);
    ix = make_ix(INDICES, 3, &native);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.mint_only.payer, 1));
    TEST_ASSERT(is_account(decoded.mint_only.mint, 2));

    payload excess = {{0}, 0};
    put_u8(&excess, IDX_TOKEN_2022_IX_WITHDRAW_EXCESS_LAMPORTS);
    ix = make_ix(INDICES, 3, &excess);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.withdraw_excess_lamports.source, 1));
    TEST_ASSERT(is_account(decoded.withdraw_excess_lamports.destination, 2));
    TEST_ASSERT(is_account(decoded.withdraw_excess_lamports.authority, 3));
}

static void test_reallocate(void) {
    fixture f;
    fixture_init(&f);
    idx_token_2022_instruction decoded;

    /* Account 2 is the system program, so the owner is the fourth. */
    payload data = {{0}, 0};
    put_u8(&data, IDX_TOKEN_2022_IX_REALLOCATE);
    put_u16(&data, 7);  /* MemoTransfer */
    put_u16(&data, 12); /* CpiGuard */
    idx_instruction ix = make_ix(INDICES, 4, &data);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.reallocate.account, 1));
    TEST_ASSERT(is_account(decoded.reallocate.payer, 2));
    TEST_ASSERT(is_account(decoded.reallocate.owner, 4));
    TEST_EQ_UINT(decoded.reallocate.extension_type_count, 2);
    TEST_EQ_UINT(
        idx_token_2022_extension_type_at(decoded.reallocate.extension_types, 0),
        7);
    TEST_EQ_UINT(
        idx_token_2022_extension_type_at(decoded.reallocate.extension_types, 1),
        12);
    /* Past the end reads as the type the program calls Uninitialized. */
    TEST_EQ_UINT(
        idx_token_2022_extension_type_at(decoded.reallocate.extension_types, 2),
        0);

    /* A reallocate that names nothing is still well formed. */
    payload empty = {{0}, 0};
    put_u8(&empty, IDX_TOKEN_2022_IX_REALLOCATE);
    ix = make_ix(INDICES, 4, &empty);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_UINT(decoded.reallocate.extension_type_count, 0);

    /* Half a type is malformed. */
    payload odd = {{0}, 0};
    put_u8(&odd, IDX_TOKEN_2022_IX_REALLOCATE);
    put_u8(&odd, 7);
    ix = make_ix(INDICES, 4, &odd);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);

    /* The owner is the fourth account, so three are not enough. */
    ix = make_ix(INDICES, 3, &data);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);
}

/*
 * An extension group is identified and its payload handed over undecoded,
 * which is what decision D5 asks for.
 */
static void test_extension_groups(void) {
    fixture f;
    fixture_init(&f);
    idx_token_2022_instruction decoded;

    /* TransferFeeExtension / TransferCheckedWithFee, the sub-instruction a
     * transfer consumer will want first. */
    payload fee = {{0}, 0};
    put_u8(&fee, IDX_TOKEN_2022_IX_TRANSFER_FEE_EXTENSION);
    put_u8(&fee, 1);
    put_u64(&fee, 1000);
    put_u8(&fee, 6);
    put_u64(&fee, 25);
    idx_instruction ix = make_ix(INDICES, 4, &fee);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(!decoded.is_base);
    TEST_EQ_INT(decoded.kind, IDX_TOKEN_2022_IX_TRANSFER_FEE_EXTENSION);
    TEST_ASSERT(idx_token_2022_ix_is_extension(decoded.kind));
    TEST_EQ_UINT(decoded.extension.sub_discriminant, 1);
    TEST_EQ_UINT(decoded.extension.payload.len, 17);

    /* A group whose sub-instruction carries nothing. */
    payload memo = {{0}, 0};
    put_u8(&memo, IDX_TOKEN_2022_IX_MEMO_TRANSFER_EXTENSION);
    put_u8(&memo, 0);
    ix = make_ix(INDICES, 2, &memo);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_UINT(decoded.extension.sub_discriminant, 0);
    TEST_EQ_UINT(decoded.extension.payload.len, 0);

    payload metadata_pointer = {{0}, 0};
    put_u8(&metadata_pointer, IDX_TOKEN_2022_IX_METADATA_POINTER_EXTENSION);
    put_u8(&metadata_pointer, 0);
    put_key(&metadata_pointer, 0xd1);
    ix = make_ix(INDICES, 1, &metadata_pointer);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_INT(decoded.kind, IDX_TOKEN_2022_IX_METADATA_POINTER_EXTENSION);
    TEST_EQ_UINT(decoded.extension.payload.len, IDX_PUBKEY_LEN);

    /* A group discriminant with no sub-instruction byte is truncated. */
    payload bare = {{0}, 0};
    put_u8(&bare, IDX_TOKEN_2022_IX_PAUSABLE_EXTENSION);
    ix = make_ix(INDICES, 1, &bare);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_RANGE);
}

static void test_errors(void) {
    fixture f;
    fixture_init(&f);
    idx_token_2022_instruction decoded;

    /* Past the last known discriminant: an upgrade, or one of the eight-byte
     * metadata interface discriminators. */
    payload unknown = {{0}, 0};
    put_u8(&unknown, IDX_TOKEN_2022_IX_PAUSABLE_EXTENSION + 1);
    idx_instruction ix = make_ix(INDICES, 3, &unknown);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_NOT_FOUND);

    payload high = {{0}, 0};
    put_u8(&high, 255);
    ix = make_ix(INDICES, 3, &high);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_NOT_FOUND);

    payload empty = {{0}, 0};
    ix = make_ix(INDICES, 3, &empty);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_RANGE);

    /* A permanent delegate whose pubkey is cut short. */
    payload truncated = {{0}, 0};
    put_u8(&truncated, IDX_TOKEN_2022_IX_INITIALIZE_PERMANENT_DELEGATE);
    put_u8(&truncated, 1);
    ix = make_ix(INDICES, 1, &truncated);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_RANGE);

    payload native = {{0}, 0};
    put_u8(&native, IDX_TOKEN_2022_IX_CREATE_NATIVE_MINT);
    ix = make_ix(INDICES, 1, &native);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);

    ix = make_ix(INDICES, 3, &native);
    TEST_EQ_INT(idx_token_2022_instruction_decode(NULL, &ix, &decoded, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_token_2022_instruction_decode(&f.tx, NULL, &decoded, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_token_2022_instruction_decode(&f.tx, &ix, NULL, NULL),
                IDX_ERR_INVALID_ARG);
}

static void test_names_and_classification(void) {
    TEST_EQ_STR(idx_token_2022_ix_kind_name(IDX_TOKEN_2022_IX_REALLOCATE),
                "Reallocate");
    TEST_EQ_STR(
        idx_token_2022_ix_kind_name(IDX_TOKEN_2022_IX_TRANSFER_FEE_EXTENSION),
        "TransferFeeExtension");
    TEST_EQ_STR(idx_token_2022_ix_kind_name((idx_token_2022_ix_kind)200),
                "unknown");

    /* The instructions decoded in full are not extension groups. */
    TEST_ASSERT(!idx_token_2022_ix_is_extension(IDX_TOKEN_2022_IX_REALLOCATE));
    TEST_ASSERT(
        !idx_token_2022_ix_is_extension(IDX_TOKEN_2022_IX_CREATE_NATIVE_MINT));
    TEST_ASSERT(idx_token_2022_ix_is_extension(
        IDX_TOKEN_2022_IX_CONFIDENTIAL_TRANSFER_EXTENSION));
    TEST_ASSERT(
        idx_token_2022_ix_is_extension(IDX_TOKEN_2022_IX_PAUSABLE_EXTENSION));

    /* An empty type list reads as Uninitialized rather than out of bounds. */
    TEST_EQ_UINT(idx_token_2022_extension_type_at(idx_slice_make(NULL, 0), 0),
                 0);
}

TEST_MAIN({
    TEST_RUN(test_base_instructions_pass_through);
    TEST_RUN(test_initialize_mint_close_authority);
    TEST_RUN(test_mint_variants);
    TEST_RUN(test_reallocate);
    TEST_RUN(test_extension_groups);
    TEST_RUN(test_errors);
    TEST_RUN(test_names_and_classification);
})
