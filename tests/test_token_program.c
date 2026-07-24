/*
 * SPL Token instruction decoding. The fixtures are the packed bytes the token
 * program writes, built here rather than taken from a block, so the layout is
 * asserted directly. Account operands are checked by identity: account `i` of
 * the fixture transaction is the key whose every byte is `i`.
 */
#include "token_program.h"

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

static void put_u64(payload *p, uint64_t value) {
    for (size_t i = 0; i < 8; i++) {
        put_u8(p, (uint8_t)((value >> (8 * i)) & 0xff));
    }
}

static void put_key(payload *p, uint8_t fill) {
    memset(p->bytes + p->len, fill, IDX_PUBKEY_LEN);
    p->len += IDX_PUBKEY_LEN;
}

/* A COption<Pubkey>: the tag, then the key when there is one. */
static void put_optional_key(payload *p, bool present, uint8_t fill) {
    put_u8(p, present ? 1 : 0);
    if (present) {
        put_key(p, fill);
    }
}

static void put_text(payload *p, const char *text) {
    size_t len = strlen(text);
    memcpy(p->bytes + p->len, text, len);
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

static idx_status decode(const fixture *f, const idx_instruction *ix,
                         idx_token_instruction *out) {
    idx_error err;
    idx_error_clear(&err);
    idx_status status = idx_token_instruction_decode(&f->tx, ix, out, &err);
    if (status != IDX_OK) {
        TEST_CHECK(err.file != NULL, "failure recorded no context");
    }
    return status;
}

/* The account list every fixture instruction draws from, longest first. */
static const uint8_t INDICES[] = {1, 2, 3, 4, 5};

/* ---------------------------------------------------------------- tests -- */

static void test_transfer(void) {
    fixture f;
    fixture_init(&f);
    idx_token_instruction decoded;

    payload data = {{0}, 0};
    put_u8(&data, IDX_TOKEN_IX_TRANSFER);
    put_u64(&data, 1500000);
    idx_instruction ix = make_ix(INDICES, 3, &data);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_INT(decoded.kind, IDX_TOKEN_IX_TRANSFER);
    TEST_ASSERT(is_account(decoded.transfer.source, 1));
    TEST_ASSERT(is_account(decoded.transfer.destination, 2));
    TEST_ASSERT(is_account(decoded.transfer.authority, 3));
    TEST_ASSERT(decoded.transfer.mint == NULL);
    TEST_ASSERT(!decoded.transfer.has_decimals);
    TEST_EQ_UINT(decoded.transfer.amount, 1500000);

    /* The checked form names the mint second, which shifts everything after
     * it, and states the scale the program verifies. */
    payload checked = {{0}, 0};
    put_u8(&checked, IDX_TOKEN_IX_TRANSFER_CHECKED);
    put_u64(&checked, 250);
    put_u8(&checked, 6);
    ix = make_ix(INDICES, 4, &checked);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_INT(decoded.kind, IDX_TOKEN_IX_TRANSFER_CHECKED);
    TEST_ASSERT(is_account(decoded.transfer.source, 1));
    TEST_ASSERT(is_account(decoded.transfer.mint, 2));
    TEST_ASSERT(is_account(decoded.transfer.destination, 3));
    TEST_ASSERT(is_account(decoded.transfer.authority, 4));
    TEST_ASSERT(decoded.transfer.has_decimals);
    TEST_EQ_UINT(decoded.transfer.decimals, 6);
    TEST_EQ_UINT(decoded.transfer.amount, 250);
}

/* A multisig authority is followed by its signers, which the decode ignores
 * without failing. */
static void test_transfer_with_multisig_signers(void) {
    fixture f;
    fixture_init(&f);
    idx_token_instruction decoded;

    payload data = {{0}, 0};
    put_u8(&data, IDX_TOKEN_IX_TRANSFER);
    put_u64(&data, 7);
    idx_instruction ix = make_ix(INDICES, 5, &data);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.transfer.authority, 3));
    TEST_EQ_UINT(decoded.transfer.amount, 7);
}

static void test_approve_and_revoke(void) {
    fixture f;
    fixture_init(&f);
    idx_token_instruction decoded;

    payload approve = {{0}, 0};
    put_u8(&approve, IDX_TOKEN_IX_APPROVE);
    put_u64(&approve, 900);
    idx_instruction ix = make_ix(INDICES, 3, &approve);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.approve.source, 1));
    TEST_ASSERT(is_account(decoded.approve.delegate, 2));
    TEST_ASSERT(is_account(decoded.approve.owner, 3));
    TEST_ASSERT(decoded.approve.mint == NULL);
    TEST_EQ_UINT(decoded.approve.amount, 900);

    payload checked = {{0}, 0};
    put_u8(&checked, IDX_TOKEN_IX_APPROVE_CHECKED);
    put_u64(&checked, 900);
    put_u8(&checked, 9);
    ix = make_ix(INDICES, 4, &checked);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.approve.source, 1));
    TEST_ASSERT(is_account(decoded.approve.mint, 2));
    TEST_ASSERT(is_account(decoded.approve.delegate, 3));
    TEST_ASSERT(is_account(decoded.approve.owner, 4));
    TEST_EQ_UINT(decoded.approve.decimals, 9);

    payload revoke = {{0}, 0};
    put_u8(&revoke, IDX_TOKEN_IX_REVOKE);
    ix = make_ix(INDICES, 2, &revoke);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.revoke.source, 1));
    TEST_ASSERT(is_account(decoded.revoke.owner, 2));
}

static void test_mint_to_and_burn(void) {
    fixture f;
    fixture_init(&f);
    idx_token_instruction decoded;

    payload mint_to = {{0}, 0};
    put_u8(&mint_to, IDX_TOKEN_IX_MINT_TO);
    put_u64(&mint_to, 10);
    idx_instruction ix = make_ix(INDICES, 3, &mint_to);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.mint_to.mint, 1));
    TEST_ASSERT(is_account(decoded.mint_to.account, 2));
    TEST_ASSERT(is_account(decoded.mint_to.authority, 3));
    TEST_ASSERT(!decoded.mint_to.has_decimals);
    TEST_EQ_UINT(decoded.mint_to.amount, 10);

    /* The checked form keeps the account order and appends the scale. */
    payload mint_to_checked = {{0}, 0};
    put_u8(&mint_to_checked, IDX_TOKEN_IX_MINT_TO_CHECKED);
    put_u64(&mint_to_checked, 11);
    put_u8(&mint_to_checked, 2);
    ix = make_ix(INDICES, 3, &mint_to_checked);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.mint_to.mint, 1));
    TEST_ASSERT(decoded.mint_to.has_decimals);
    TEST_EQ_UINT(decoded.mint_to.decimals, 2);

    /* Burn reverses mint's first two accounts. */
    payload burn = {{0}, 0};
    put_u8(&burn, IDX_TOKEN_IX_BURN);
    put_u64(&burn, 12);
    ix = make_ix(INDICES, 3, &burn);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.burn.account, 1));
    TEST_ASSERT(is_account(decoded.burn.mint, 2));
    TEST_ASSERT(is_account(decoded.burn.authority, 3));
    TEST_EQ_UINT(decoded.burn.amount, 12);

    payload burn_checked = {{0}, 0};
    put_u8(&burn_checked, IDX_TOKEN_IX_BURN_CHECKED);
    put_u64(&burn_checked, 13);
    put_u8(&burn_checked, 8);
    ix = make_ix(INDICES, 3, &burn_checked);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(decoded.burn.has_decimals);
    TEST_EQ_UINT(decoded.burn.decimals, 8);
}

static void test_set_authority(void) {
    fixture f;
    fixture_init(&f);
    idx_token_instruction decoded;

    payload data = {{0}, 0};
    put_u8(&data, IDX_TOKEN_IX_SET_AUTHORITY);
    put_u8(&data, IDX_TOKEN_AUTHORITY_ACCOUNT_OWNER);
    put_optional_key(&data, true, 0x9a);
    idx_instruction ix = make_ix(INDICES, 2, &data);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.set_authority.account, 1));
    TEST_ASSERT(is_account(decoded.set_authority.authority, 2));
    TEST_EQ_UINT(decoded.set_authority.authority_type,
                 IDX_TOKEN_AUTHORITY_ACCOUNT_OWNER);
    TEST_ASSERT(decoded.set_authority.has_new_authority);
    TEST_ASSERT(is_filled_key(&decoded.set_authority.new_authority, 0x9a));

    /* No new authority revokes the old one. */
    payload revoking = {{0}, 0};
    put_u8(&revoking, IDX_TOKEN_IX_SET_AUTHORITY);
    put_u8(&revoking, IDX_TOKEN_AUTHORITY_FREEZE_ACCOUNT);
    put_optional_key(&revoking, false, 0);
    ix = make_ix(INDICES, 2, &revoking);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(!decoded.set_authority.has_new_authority);

    /* Token-2022 numbers further authority types; the byte is passed through
     * rather than rejected. */
    payload extension = {{0}, 0};
    put_u8(&extension, IDX_TOKEN_IX_SET_AUTHORITY);
    put_u8(&extension, 12);
    put_optional_key(&extension, false, 0);
    ix = make_ix(INDICES, 2, &extension);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_UINT(decoded.set_authority.authority_type, 12);

    /* A tag that is neither 0 nor 1 is malformed. */
    payload bad_tag = {{0}, 0};
    put_u8(&bad_tag, IDX_TOKEN_IX_SET_AUTHORITY);
    put_u8(&bad_tag, IDX_TOKEN_AUTHORITY_MINT_TOKENS);
    put_u8(&bad_tag, 2);
    ix = make_ix(INDICES, 2, &bad_tag);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);
}

static void test_close_freeze_thaw(void) {
    fixture f;
    fixture_init(&f);
    idx_token_instruction decoded;

    payload close = {{0}, 0};
    put_u8(&close, IDX_TOKEN_IX_CLOSE_ACCOUNT);
    idx_instruction ix = make_ix(INDICES, 3, &close);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.close_account.account, 1));
    TEST_ASSERT(is_account(decoded.close_account.destination, 2));
    TEST_ASSERT(is_account(decoded.close_account.owner, 3));

    payload freeze = {{0}, 0};
    put_u8(&freeze, IDX_TOKEN_IX_FREEZE_ACCOUNT);
    ix = make_ix(INDICES, 3, &freeze);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_INT(decoded.kind, IDX_TOKEN_IX_FREEZE_ACCOUNT);
    TEST_ASSERT(is_account(decoded.freeze_account.account, 1));
    TEST_ASSERT(is_account(decoded.freeze_account.mint, 2));
    TEST_ASSERT(is_account(decoded.freeze_account.authority, 3));

    payload thaw = {{0}, 0};
    put_u8(&thaw, IDX_TOKEN_IX_THAW_ACCOUNT);
    ix = make_ix(INDICES, 3, &thaw);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_INT(decoded.kind, IDX_TOKEN_IX_THAW_ACCOUNT);
    TEST_ASSERT(is_account(decoded.freeze_account.account, 1));
}

static void test_initialize_mint(void) {
    fixture f;
    fixture_init(&f);
    idx_token_instruction decoded;

    payload data = {{0}, 0};
    put_u8(&data, IDX_TOKEN_IX_INITIALIZE_MINT);
    put_u8(&data, 6);
    put_key(&data, 0xa1);
    put_optional_key(&data, true, 0xa2);
    idx_instruction ix = make_ix(INDICES, 2, &data);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.initialize_mint.mint, 1));
    TEST_EQ_UINT(decoded.initialize_mint.decimals, 6);
    TEST_ASSERT(is_filled_key(&decoded.initialize_mint.mint_authority, 0xa1));
    TEST_ASSERT(decoded.initialize_mint.has_freeze_authority);
    TEST_ASSERT(
        is_filled_key(&decoded.initialize_mint.freeze_authority, 0xa2));

    /* The 2 form drops the rent sysvar account and keeps the layout. */
    payload two = {{0}, 0};
    put_u8(&two, IDX_TOKEN_IX_INITIALIZE_MINT2);
    put_u8(&two, 0);
    put_key(&two, 0xa3);
    put_optional_key(&two, false, 0);
    ix = make_ix(INDICES, 1, &two);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_INT(decoded.kind, IDX_TOKEN_IX_INITIALIZE_MINT2);
    TEST_ASSERT(is_account(decoded.initialize_mint.mint, 1));
    TEST_EQ_UINT(decoded.initialize_mint.decimals, 0);
    TEST_ASSERT(!decoded.initialize_mint.has_freeze_authority);
}

static void test_initialize_account(void) {
    fixture f;
    fixture_init(&f);
    idx_token_instruction decoded;

    /* The original form names the owner as an account. */
    payload data = {{0}, 0};
    put_u8(&data, IDX_TOKEN_IX_INITIALIZE_ACCOUNT);
    idx_instruction ix = make_ix(INDICES, 4, &data);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.initialize_account.account, 1));
    TEST_ASSERT(is_account(decoded.initialize_account.mint, 2));
    TEST_ASSERT(is_account(decoded.initialize_account.owner_account, 3));
    TEST_ASSERT(is_account(&decoded.initialize_account.owner, 3));

    /* The 2 and 3 forms carry it in the data instead. */
    payload two = {{0}, 0};
    put_u8(&two, IDX_TOKEN_IX_INITIALIZE_ACCOUNT2);
    put_key(&two, 0xb1);
    ix = make_ix(INDICES, 3, &two);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(decoded.initialize_account.owner_account == NULL);
    TEST_ASSERT(is_filled_key(&decoded.initialize_account.owner, 0xb1));

    payload three = {{0}, 0};
    put_u8(&three, IDX_TOKEN_IX_INITIALIZE_ACCOUNT3);
    put_key(&three, 0xb2);
    ix = make_ix(INDICES, 2, &three);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.initialize_account.account, 1));
    TEST_ASSERT(is_account(decoded.initialize_account.mint, 2));
    TEST_ASSERT(is_filled_key(&decoded.initialize_account.owner, 0xb2));
}

static void test_initialize_multisig(void) {
    fixture f;
    fixture_init(&f);
    idx_token_instruction decoded;

    /* Multisig, rent sysvar, then two signers. */
    payload data = {{0}, 0};
    put_u8(&data, IDX_TOKEN_IX_INITIALIZE_MULTISIG);
    put_u8(&data, 2);
    idx_instruction ix = make_ix(INDICES, 4, &data);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.initialize_multisig.multisig, 1));
    TEST_EQ_UINT(decoded.initialize_multisig.required_signatures, 2);
    TEST_EQ_UINT(decoded.initialize_multisig.signer_count, 2);
    TEST_EQ_UINT(decoded.initialize_multisig.signers[0], 3);
    TEST_EQ_UINT(decoded.initialize_multisig.signers[1], 4);

    /* The 2 form has no rent sysvar, so the signers start one earlier. */
    payload two = {{0}, 0};
    put_u8(&two, IDX_TOKEN_IX_INITIALIZE_MULTISIG2);
    put_u8(&two, 1);
    ix = make_ix(INDICES, 3, &two);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_UINT(decoded.initialize_multisig.signer_count, 2);
    TEST_EQ_UINT(decoded.initialize_multisig.signers[0], 2);

    /* A multisig with no signer accounts decodes to an empty list rather
     * than failing; the program is what rejects it. */
    ix = make_ix(INDICES, 1, &two);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_EQ_UINT(decoded.initialize_multisig.signer_count, 0);
    TEST_ASSERT(decoded.initialize_multisig.signers == NULL);
}

static void test_single_account_variants(void) {
    fixture f;
    fixture_init(&f);
    idx_token_instruction decoded;

    payload sync = {{0}, 0};
    put_u8(&sync, IDX_TOKEN_IX_SYNC_NATIVE);
    idx_instruction ix = make_ix(INDICES, 1, &sync);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.account_only.account, 1));

    payload immutable = {{0}, 0};
    put_u8(&immutable, IDX_TOKEN_IX_INITIALIZE_IMMUTABLE_OWNER);
    ix = make_ix(INDICES, 1, &immutable);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.account_only.account, 1));

    /* Token-2022 appends the extension types to weigh, which are ignored. */
    payload data_size = {{0}, 0};
    put_u8(&data_size, IDX_TOKEN_IX_GET_ACCOUNT_DATA_SIZE);
    put_u8(&data_size, 1);
    put_u8(&data_size, 0);
    ix = make_ix(INDICES, 1, &data_size);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.mint_query.mint, 1));
    TEST_EQ_UINT(decoded.mint_query.amount, 0);

    payload to_ui = {{0}, 0};
    put_u8(&to_ui, IDX_TOKEN_IX_AMOUNT_TO_UI_AMOUNT);
    put_u64(&to_ui, 123456);
    ix = make_ix(INDICES, 1, &to_ui);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(is_account(decoded.mint_query.mint, 1));
    TEST_EQ_UINT(decoded.mint_query.amount, 123456);

    payload from_ui = {{0}, 0};
    put_u8(&from_ui, IDX_TOKEN_IX_UI_AMOUNT_TO_AMOUNT);
    put_text(&from_ui, "0.123456");
    ix = make_ix(INDICES, 1, &from_ui);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_OK);
    TEST_ASSERT(idx_slice_equal(decoded.ui_amount_to_amount.ui_amount,
                                idx_slice_from_str("0.123456")));
}

static void test_program_matches(void) {
    TEST_ASSERT(idx_token_program_matches(&IDX_PROGRAM_TOKEN));
    TEST_ASSERT(idx_token_program_matches(&IDX_PROGRAM_TOKEN_2022));
    TEST_ASSERT(!idx_token_program_matches(&IDX_PROGRAM_SYSTEM));
    TEST_ASSERT(!idx_token_program_matches(&IDX_PROGRAM_MEMO));
    TEST_ASSERT(!idx_token_program_matches(NULL));
}

static void test_errors(void) {
    fixture f;
    fixture_init(&f);
    idx_token_instruction decoded;

    /* The first Token-2022 extension discriminant. Skipping it is the
     * documented outcome until the extension decoders land. */
    payload extension = {{0}, 0};
    put_u8(&extension, IDX_TOKEN_IX_UI_AMOUNT_TO_AMOUNT + 1);
    idx_instruction ix = make_ix(INDICES, 3, &extension);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_NOT_FOUND);

    payload unknown = {{0}, 0};
    put_u8(&unknown, 255);
    ix = make_ix(INDICES, 3, &unknown);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_NOT_FOUND);

    payload empty = {{0}, 0};
    ix = make_ix(INDICES, 3, &empty);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_RANGE);

    payload truncated = {{0}, 0};
    put_u8(&truncated, IDX_TOKEN_IX_TRANSFER);
    put_u8(&truncated, 1);
    ix = make_ix(INDICES, 3, &truncated);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_RANGE);

    /* A checked transfer whose decimals byte is missing. */
    payload no_decimals = {{0}, 0};
    put_u8(&no_decimals, IDX_TOKEN_IX_TRANSFER_CHECKED);
    put_u64(&no_decimals, 1);
    ix = make_ix(INDICES, 4, &no_decimals);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_RANGE);

    payload transfer = {{0}, 0};
    put_u8(&transfer, IDX_TOKEN_IX_TRANSFER);
    put_u64(&transfer, 1);
    ix = make_ix(INDICES, 2, &transfer);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);
    ix = make_ix(INDICES, 0, &transfer);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);

    /* A checked transfer has one more account than the unchecked one. */
    payload checked = {{0}, 0};
    put_u8(&checked, IDX_TOKEN_IX_TRANSFER_CHECKED);
    put_u64(&checked, 1);
    put_u8(&checked, 6);
    ix = make_ix(INDICES, 3, &checked);
    TEST_EQ_INT(decode(&f, &ix, &decoded), IDX_ERR_PARSE);

    ix = make_ix(INDICES, 3, &transfer);
    TEST_EQ_INT(idx_token_instruction_decode(NULL, &ix, &decoded, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_token_instruction_decode(&f.tx, NULL, &decoded, NULL),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_token_instruction_decode(&f.tx, &ix, NULL, NULL),
                IDX_ERR_INVALID_ARG);
}

static void test_names(void) {
    TEST_EQ_STR(idx_token_ix_kind_name(IDX_TOKEN_IX_TRANSFER_CHECKED),
                "TransferChecked");
    TEST_EQ_STR(idx_token_ix_kind_name(IDX_TOKEN_IX_INITIALIZE_MINT2),
                "InitializeMint2");
    TEST_EQ_STR(idx_token_ix_kind_name((idx_token_ix_kind)200), "unknown");
    TEST_EQ_STR(
        idx_token_authority_type_name(IDX_TOKEN_AUTHORITY_CLOSE_ACCOUNT),
        "CloseAccount");
    TEST_EQ_STR(idx_token_authority_type_name(200), "unknown");
}

TEST_MAIN({
    TEST_RUN(test_transfer);
    TEST_RUN(test_transfer_with_multisig_signers);
    TEST_RUN(test_approve_and_revoke);
    TEST_RUN(test_mint_to_and_burn);
    TEST_RUN(test_set_authority);
    TEST_RUN(test_close_freeze_thaw);
    TEST_RUN(test_initialize_mint);
    TEST_RUN(test_initialize_account);
    TEST_RUN(test_initialize_multisig);
    TEST_RUN(test_single_account_variants);
    TEST_RUN(test_program_matches);
    TEST_RUN(test_errors);
    TEST_RUN(test_names);
})
