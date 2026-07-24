#include "system_program.h"

#include <string.h>

const char *idx_system_ix_kind_name(idx_system_ix_kind kind) {
    switch (kind) {
    case IDX_SYSTEM_IX_CREATE_ACCOUNT:
        return "CreateAccount";
    case IDX_SYSTEM_IX_ASSIGN:
        return "Assign";
    case IDX_SYSTEM_IX_TRANSFER:
        return "Transfer";
    case IDX_SYSTEM_IX_CREATE_ACCOUNT_WITH_SEED:
        return "CreateAccountWithSeed";
    case IDX_SYSTEM_IX_ADVANCE_NONCE_ACCOUNT:
        return "AdvanceNonceAccount";
    case IDX_SYSTEM_IX_WITHDRAW_NONCE_ACCOUNT:
        return "WithdrawNonceAccount";
    case IDX_SYSTEM_IX_INITIALIZE_NONCE_ACCOUNT:
        return "InitializeNonceAccount";
    case IDX_SYSTEM_IX_AUTHORIZE_NONCE_ACCOUNT:
        return "AuthorizeNonceAccount";
    case IDX_SYSTEM_IX_ALLOCATE:
        return "Allocate";
    case IDX_SYSTEM_IX_ALLOCATE_WITH_SEED:
        return "AllocateWithSeed";
    case IDX_SYSTEM_IX_ASSIGN_WITH_SEED:
        return "AssignWithSeed";
    case IDX_SYSTEM_IX_TRANSFER_WITH_SEED:
        return "TransferWithSeed";
    case IDX_SYSTEM_IX_UPGRADE_NONCE_ACCOUNT:
        return "UpgradeNonceAccount";
    }
    return "unknown";
}

/* Resolves a named operand, failing with the variant's name when the
 * instruction is short of accounts. */
static idx_status require_account(const idx_transaction *tx,
                                  const idx_instruction *ix, size_t position,
                                  idx_system_ix_kind kind,
                                  const idx_pubkey **out, idx_error *err) {
    const idx_pubkey *account = idx_instruction_account(tx, ix, position);
    if (account == NULL) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "system %s needs account %zu, instruction has %zu",
                        idx_system_ix_kind_name(kind), position,
                        ix->account_count);
    }
    *out = account;
    return IDX_OK;
}

static idx_status read_pubkey(idx_cursor *cursor, idx_pubkey *out,
                              idx_error *err) {
    return idx_cursor_copy(cursor, out->bytes, IDX_PUBKEY_LEN, err);
}

/*
 * A bincode `String`: a little-endian u64 length, then that many bytes. The
 * length is checked against what remains before the cast to size_t, so a
 * hostile value cannot wrap on a 32-bit target.
 */
static idx_status read_seed(idx_cursor *cursor, idx_slice *out,
                            idx_error *err) {
    uint64_t len = 0;
    IDX_TRY(idx_cursor_u64le(cursor, &len, err));
    if (len > (uint64_t)idx_cursor_remaining(cursor)) {
        return IDX_FAIL(err, IDX_ERR_RANGE,
                        "seed length %llu exceeds the %zu bytes remaining",
                        (unsigned long long)len, idx_cursor_remaining(cursor));
    }
    return idx_cursor_take(cursor, (size_t)len, out, err);
}

idx_status idx_system_instruction_decode(const idx_transaction *tx,
                                         const idx_instruction *ix,
                                         idx_system_instruction *out,
                                         idx_error *err) {
    if (tx == NULL || ix == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, ix and out must not be NULL");
    }

    idx_cursor cursor;
    idx_cursor_init(&cursor, ix->data);
    uint32_t discriminant = 0;
    IDX_TRY(idx_cursor_u32le(&cursor, &discriminant, err));
    if (discriminant > (uint32_t)IDX_SYSTEM_IX_UPGRADE_NONCE_ACCOUNT) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND,
                        "unknown system instruction %u",
                        (unsigned)discriminant);
    }

    memset(out, 0, sizeof(*out));
    idx_system_ix_kind kind = (idx_system_ix_kind)discriminant;
    out->kind = kind;

    switch (kind) {
    case IDX_SYSTEM_IX_CREATE_ACCOUNT:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->create_account.funder,
                                err));
        IDX_TRY(require_account(tx, ix, 1, kind, &out->create_account.account,
                                err));
        IDX_TRY(idx_cursor_u64le(&cursor, &out->create_account.lamports, err));
        IDX_TRY(idx_cursor_u64le(&cursor, &out->create_account.space, err));
        return read_pubkey(&cursor, &out->create_account.owner, err);

    case IDX_SYSTEM_IX_ASSIGN:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->assign.account, err));
        return read_pubkey(&cursor, &out->assign.owner, err);

    case IDX_SYSTEM_IX_TRANSFER:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->transfer.from, err));
        IDX_TRY(require_account(tx, ix, 1, kind, &out->transfer.to, err));
        return idx_cursor_u64le(&cursor, &out->transfer.lamports, err);

    case IDX_SYSTEM_IX_CREATE_ACCOUNT_WITH_SEED:
        /* The base is a third account only when it is not the funder, so it is
         * not resolved here; `base` from the data identifies it either way. */
        IDX_TRY(require_account(tx, ix, 0, kind,
                                &out->create_account_with_seed.funder, err));
        IDX_TRY(require_account(tx, ix, 1, kind,
                                &out->create_account_with_seed.account, err));
        IDX_TRY(read_pubkey(&cursor, &out->create_account_with_seed.base,
                            err));
        IDX_TRY(read_seed(&cursor, &out->create_account_with_seed.seed, err));
        IDX_TRY(idx_cursor_u64le(
            &cursor, &out->create_account_with_seed.lamports, err));
        IDX_TRY(idx_cursor_u64le(&cursor, &out->create_account_with_seed.space,
                                 err));
        return read_pubkey(&cursor, &out->create_account_with_seed.owner, err);

    case IDX_SYSTEM_IX_ADVANCE_NONCE_ACCOUNT:
        /* Account 1 is the recent blockhashes sysvar, which carries nothing
         * this indexer keeps. */
        IDX_TRY(require_account(tx, ix, 0, kind, &out->nonce.nonce, err));
        return require_account(tx, ix, 2, kind, &out->nonce.authority, err);

    case IDX_SYSTEM_IX_UPGRADE_NONCE_ACCOUNT:
        return require_account(tx, ix, 0, kind, &out->nonce.nonce, err);

    case IDX_SYSTEM_IX_WITHDRAW_NONCE_ACCOUNT:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->withdraw_nonce.nonce,
                                err));
        IDX_TRY(require_account(tx, ix, 1, kind, &out->withdraw_nonce.to,
                                err));
        /* Accounts 2 and 3 are the blockhashes and rent sysvars. */
        IDX_TRY(require_account(tx, ix, 4, kind,
                                &out->withdraw_nonce.authority, err));
        return idx_cursor_u64le(&cursor, &out->withdraw_nonce.lamports, err);

    case IDX_SYSTEM_IX_INITIALIZE_NONCE_ACCOUNT:
        IDX_TRY(require_account(tx, ix, 0, kind,
                                &out->set_nonce_authority.nonce, err));
        return read_pubkey(&cursor, &out->set_nonce_authority.new_authority,
                           err);

    case IDX_SYSTEM_IX_AUTHORIZE_NONCE_ACCOUNT:
        IDX_TRY(require_account(tx, ix, 0, kind,
                                &out->set_nonce_authority.nonce, err));
        IDX_TRY(require_account(tx, ix, 1, kind,
                                &out->set_nonce_authority.authority, err));
        return read_pubkey(&cursor, &out->set_nonce_authority.new_authority,
                           err);

    case IDX_SYSTEM_IX_ALLOCATE:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->allocate.account, err));
        return idx_cursor_u64le(&cursor, &out->allocate.space, err);

    case IDX_SYSTEM_IX_ALLOCATE_WITH_SEED:
        IDX_TRY(require_account(tx, ix, 0, kind,
                                &out->allocate_with_seed.account, err));
        IDX_TRY(require_account(tx, ix, 1, kind,
                                &out->allocate_with_seed.base_account, err));
        IDX_TRY(read_pubkey(&cursor, &out->allocate_with_seed.base, err));
        IDX_TRY(read_seed(&cursor, &out->allocate_with_seed.seed, err));
        IDX_TRY(idx_cursor_u64le(&cursor, &out->allocate_with_seed.space,
                                 err));
        return read_pubkey(&cursor, &out->allocate_with_seed.owner, err);

    case IDX_SYSTEM_IX_ASSIGN_WITH_SEED:
        IDX_TRY(require_account(tx, ix, 0, kind,
                                &out->assign_with_seed.account, err));
        IDX_TRY(require_account(tx, ix, 1, kind,
                                &out->assign_with_seed.base_account, err));
        IDX_TRY(read_pubkey(&cursor, &out->assign_with_seed.base, err));
        IDX_TRY(read_seed(&cursor, &out->assign_with_seed.seed, err));
        return read_pubkey(&cursor, &out->assign_with_seed.owner, err);

    case IDX_SYSTEM_IX_TRANSFER_WITH_SEED:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->transfer_with_seed.from,
                                err));
        IDX_TRY(require_account(tx, ix, 1, kind,
                                &out->transfer_with_seed.base_account, err));
        IDX_TRY(require_account(tx, ix, 2, kind, &out->transfer_with_seed.to,
                                err));
        IDX_TRY(idx_cursor_u64le(&cursor, &out->transfer_with_seed.lamports,
                                 err));
        IDX_TRY(read_seed(&cursor, &out->transfer_with_seed.from_seed, err));
        return read_pubkey(&cursor, &out->transfer_with_seed.from_owner, err);
    }

    return IDX_FAIL(err, IDX_ERR_INTERNAL, "unhandled system instruction %u",
                    (unsigned)discriminant);
}
