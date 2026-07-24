#include "token_program.h"

#include <string.h>

const char *idx_token_ix_kind_name(idx_token_ix_kind kind) {
    switch (kind) {
    case IDX_TOKEN_IX_INITIALIZE_MINT:
        return "InitializeMint";
    case IDX_TOKEN_IX_INITIALIZE_ACCOUNT:
        return "InitializeAccount";
    case IDX_TOKEN_IX_INITIALIZE_MULTISIG:
        return "InitializeMultisig";
    case IDX_TOKEN_IX_TRANSFER:
        return "Transfer";
    case IDX_TOKEN_IX_APPROVE:
        return "Approve";
    case IDX_TOKEN_IX_REVOKE:
        return "Revoke";
    case IDX_TOKEN_IX_SET_AUTHORITY:
        return "SetAuthority";
    case IDX_TOKEN_IX_MINT_TO:
        return "MintTo";
    case IDX_TOKEN_IX_BURN:
        return "Burn";
    case IDX_TOKEN_IX_CLOSE_ACCOUNT:
        return "CloseAccount";
    case IDX_TOKEN_IX_FREEZE_ACCOUNT:
        return "FreezeAccount";
    case IDX_TOKEN_IX_THAW_ACCOUNT:
        return "ThawAccount";
    case IDX_TOKEN_IX_TRANSFER_CHECKED:
        return "TransferChecked";
    case IDX_TOKEN_IX_APPROVE_CHECKED:
        return "ApproveChecked";
    case IDX_TOKEN_IX_MINT_TO_CHECKED:
        return "MintToChecked";
    case IDX_TOKEN_IX_BURN_CHECKED:
        return "BurnChecked";
    case IDX_TOKEN_IX_INITIALIZE_ACCOUNT2:
        return "InitializeAccount2";
    case IDX_TOKEN_IX_SYNC_NATIVE:
        return "SyncNative";
    case IDX_TOKEN_IX_INITIALIZE_ACCOUNT3:
        return "InitializeAccount3";
    case IDX_TOKEN_IX_INITIALIZE_MULTISIG2:
        return "InitializeMultisig2";
    case IDX_TOKEN_IX_INITIALIZE_MINT2:
        return "InitializeMint2";
    case IDX_TOKEN_IX_GET_ACCOUNT_DATA_SIZE:
        return "GetAccountDataSize";
    case IDX_TOKEN_IX_INITIALIZE_IMMUTABLE_OWNER:
        return "InitializeImmutableOwner";
    case IDX_TOKEN_IX_AMOUNT_TO_UI_AMOUNT:
        return "AmountToUiAmount";
    case IDX_TOKEN_IX_UI_AMOUNT_TO_AMOUNT:
        return "UiAmountToAmount";
    }
    return "unknown";
}

const char *idx_token_authority_type_name(uint8_t type) {
    switch (type) {
    case IDX_TOKEN_AUTHORITY_MINT_TOKENS:
        return "MintTokens";
    case IDX_TOKEN_AUTHORITY_FREEZE_ACCOUNT:
        return "FreezeAccount";
    case IDX_TOKEN_AUTHORITY_ACCOUNT_OWNER:
        return "AccountOwner";
    case IDX_TOKEN_AUTHORITY_CLOSE_ACCOUNT:
        return "CloseAccount";
    default:
        return "unknown";
    }
}

bool idx_token_program_matches(const idx_pubkey *program_id) {
    if (program_id == NULL) {
        return false;
    }
    return idx_pubkey_equal(program_id, &IDX_PROGRAM_TOKEN) ||
           idx_pubkey_equal(program_id, &IDX_PROGRAM_TOKEN_2022);
}

/* Resolves a named operand, failing with the variant's name when the
 * instruction is short of accounts. */
static idx_status require_account(const idx_transaction *tx,
                                  const idx_instruction *ix, size_t position,
                                  idx_token_ix_kind kind,
                                  const idx_pubkey **out, idx_error *err) {
    const idx_pubkey *account = idx_instruction_account(tx, ix, position);
    if (account == NULL) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "token %s needs account %zu, instruction has %zu",
                        idx_token_ix_kind_name(kind), position,
                        ix->account_count);
    }
    *out = account;
    return IDX_OK;
}

static idx_status read_pubkey(idx_cursor *cursor, idx_pubkey *out,
                              idx_error *err) {
    return idx_cursor_copy(cursor, out->bytes, IDX_PUBKEY_LEN, err);
}

/* A `COption<Pubkey>` as the program packs it: a one-byte tag, then the key
 * when the tag is 1. Any other tag is malformed rather than absent. */
static idx_status read_optional_pubkey(idx_cursor *cursor, idx_pubkey *out,
                                       bool *has_value, idx_error *err) {
    uint8_t tag = 0;
    IDX_TRY(idx_cursor_u8(cursor, &tag, err));
    if (tag == 0) {
        *has_value = false;
        return IDX_OK;
    }
    if (tag != 1) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "optional pubkey tag %u is neither 0 nor 1",
                        (unsigned)tag);
    }
    IDX_TRY(read_pubkey(cursor, out, err));
    *has_value = true;
    return IDX_OK;
}

/* The amount of an unchecked operation, and of a checked one with its
 * decimals. Shared by the transfer, approve, mint and burn families. */
static idx_status read_amount(idx_cursor *cursor, bool checked,
                              uint64_t *amount, uint8_t *decimals,
                              bool *has_decimals, idx_error *err) {
    IDX_TRY(idx_cursor_u64le(cursor, amount, err));
    *has_decimals = checked;
    if (!checked) {
        return IDX_OK;
    }
    return idx_cursor_u8(cursor, decimals, err);
}

/* InitializeMint and InitializeMint2, which differ only in the trailing rent
 * sysvar account that the first one takes. */
static idx_status decode_initialize_mint(const idx_transaction *tx,
                                         const idx_instruction *ix,
                                         idx_cursor *cursor,
                                         idx_token_instruction *out,
                                         idx_error *err) {
    IDX_TRY(require_account(tx, ix, 0, out->kind, &out->initialize_mint.mint,
                            err));
    IDX_TRY(idx_cursor_u8(cursor, &out->initialize_mint.decimals, err));
    IDX_TRY(read_pubkey(cursor, &out->initialize_mint.mint_authority, err));
    return read_optional_pubkey(cursor, &out->initialize_mint.freeze_authority,
                                &out->initialize_mint.has_freeze_authority,
                                err);
}

/* InitializeAccount, and the 2 and 3 forms that carry the owner in the data
 * instead of naming it as an account. */
static idx_status decode_initialize_account(const idx_transaction *tx,
                                            const idx_instruction *ix,
                                            idx_cursor *cursor,
                                            idx_token_instruction *out,
                                            idx_error *err) {
    IDX_TRY(require_account(tx, ix, 0, out->kind,
                            &out->initialize_account.account, err));
    IDX_TRY(require_account(tx, ix, 1, out->kind,
                            &out->initialize_account.mint, err));
    if (out->kind != IDX_TOKEN_IX_INITIALIZE_ACCOUNT) {
        return read_pubkey(cursor, &out->initialize_account.owner, err);
    }
    IDX_TRY(require_account(tx, ix, 2, out->kind,
                            &out->initialize_account.owner_account, err));
    out->initialize_account.owner = *out->initialize_account.owner_account;
    return IDX_OK;
}

/* InitializeMultisig and InitializeMultisig2. The signers are whatever
 * accounts follow the multisig and, for the first form, the rent sysvar. */
static idx_status decode_initialize_multisig(const idx_transaction *tx,
                                             const idx_instruction *ix,
                                             idx_cursor *cursor,
                                             idx_token_instruction *out,
                                             idx_error *err) {
    IDX_TRY(require_account(tx, ix, 0, out->kind,
                            &out->initialize_multisig.multisig, err));
    IDX_TRY(idx_cursor_u8(
        cursor, &out->initialize_multisig.required_signatures, err));

    size_t first_signer =
        (out->kind == IDX_TOKEN_IX_INITIALIZE_MULTISIG) ? 2 : 1;
    if (ix->account_count > first_signer) {
        out->initialize_multisig.signers = &ix->account_indices[first_signer];
        out->initialize_multisig.signer_count =
            ix->account_count - first_signer;
    }
    return IDX_OK;
}

idx_status idx_token_instruction_decode(const idx_transaction *tx,
                                        const idx_instruction *ix,
                                        idx_token_instruction *out,
                                        idx_error *err) {
    if (tx == NULL || ix == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, ix and out must not be NULL");
    }

    idx_cursor cursor;
    idx_cursor_init(&cursor, ix->data);
    uint8_t discriminant = 0;
    IDX_TRY(idx_cursor_u8(&cursor, &discriminant, err));
    if (discriminant > (uint8_t)IDX_TOKEN_IX_UI_AMOUNT_TO_AMOUNT) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND, "unknown token instruction %u",
                        (unsigned)discriminant);
    }

    memset(out, 0, sizeof(*out));
    idx_token_ix_kind kind = (idx_token_ix_kind)discriminant;
    out->kind = kind;

    switch (kind) {
    case IDX_TOKEN_IX_INITIALIZE_MINT:
    case IDX_TOKEN_IX_INITIALIZE_MINT2:
        return decode_initialize_mint(tx, ix, &cursor, out, err);

    case IDX_TOKEN_IX_INITIALIZE_ACCOUNT:
    case IDX_TOKEN_IX_INITIALIZE_ACCOUNT2:
    case IDX_TOKEN_IX_INITIALIZE_ACCOUNT3:
        return decode_initialize_account(tx, ix, &cursor, out, err);

    case IDX_TOKEN_IX_INITIALIZE_MULTISIG:
    case IDX_TOKEN_IX_INITIALIZE_MULTISIG2:
        return decode_initialize_multisig(tx, ix, &cursor, out, err);

    case IDX_TOKEN_IX_TRANSFER:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->transfer.source, err));
        IDX_TRY(require_account(tx, ix, 1, kind, &out->transfer.destination,
                                err));
        IDX_TRY(require_account(tx, ix, 2, kind, &out->transfer.authority,
                                err));
        return read_amount(&cursor, false, &out->transfer.amount,
                           &out->transfer.decimals,
                           &out->transfer.has_decimals, err);

    case IDX_TOKEN_IX_TRANSFER_CHECKED:
        /* The checked form inserts the mint between source and destination. */
        IDX_TRY(require_account(tx, ix, 0, kind, &out->transfer.source, err));
        IDX_TRY(require_account(tx, ix, 1, kind, &out->transfer.mint, err));
        IDX_TRY(require_account(tx, ix, 2, kind, &out->transfer.destination,
                                err));
        IDX_TRY(require_account(tx, ix, 3, kind, &out->transfer.authority,
                                err));
        return read_amount(&cursor, true, &out->transfer.amount,
                           &out->transfer.decimals,
                           &out->transfer.has_decimals, err);

    case IDX_TOKEN_IX_APPROVE:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->approve.source, err));
        IDX_TRY(require_account(tx, ix, 1, kind, &out->approve.delegate, err));
        IDX_TRY(require_account(tx, ix, 2, kind, &out->approve.owner, err));
        return read_amount(&cursor, false, &out->approve.amount,
                           &out->approve.decimals, &out->approve.has_decimals,
                           err);

    case IDX_TOKEN_IX_APPROVE_CHECKED:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->approve.source, err));
        IDX_TRY(require_account(tx, ix, 1, kind, &out->approve.mint, err));
        IDX_TRY(require_account(tx, ix, 2, kind, &out->approve.delegate, err));
        IDX_TRY(require_account(tx, ix, 3, kind, &out->approve.owner, err));
        return read_amount(&cursor, true, &out->approve.amount,
                           &out->approve.decimals, &out->approve.has_decimals,
                           err);

    case IDX_TOKEN_IX_REVOKE:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->revoke.source, err));
        return require_account(tx, ix, 1, kind, &out->revoke.owner, err);

    case IDX_TOKEN_IX_SET_AUTHORITY:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->set_authority.account,
                                err));
        IDX_TRY(require_account(tx, ix, 1, kind, &out->set_authority.authority,
                                err));
        IDX_TRY(idx_cursor_u8(&cursor, &out->set_authority.authority_type,
                              err));
        return read_optional_pubkey(&cursor,
                                    &out->set_authority.new_authority,
                                    &out->set_authority.has_new_authority,
                                    err);

    case IDX_TOKEN_IX_MINT_TO:
    case IDX_TOKEN_IX_MINT_TO_CHECKED:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->mint_to.mint, err));
        IDX_TRY(require_account(tx, ix, 1, kind, &out->mint_to.account, err));
        IDX_TRY(require_account(tx, ix, 2, kind, &out->mint_to.authority,
                                err));
        return read_amount(&cursor, kind == IDX_TOKEN_IX_MINT_TO_CHECKED,
                           &out->mint_to.amount, &out->mint_to.decimals,
                           &out->mint_to.has_decimals, err);

    case IDX_TOKEN_IX_BURN:
    case IDX_TOKEN_IX_BURN_CHECKED:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->burn.account, err));
        IDX_TRY(require_account(tx, ix, 1, kind, &out->burn.mint, err));
        IDX_TRY(require_account(tx, ix, 2, kind, &out->burn.authority, err));
        return read_amount(&cursor, kind == IDX_TOKEN_IX_BURN_CHECKED,
                           &out->burn.amount, &out->burn.decimals,
                           &out->burn.has_decimals, err);

    case IDX_TOKEN_IX_CLOSE_ACCOUNT:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->close_account.account,
                                err));
        IDX_TRY(require_account(tx, ix, 1, kind,
                                &out->close_account.destination, err));
        return require_account(tx, ix, 2, kind, &out->close_account.owner,
                               err);

    case IDX_TOKEN_IX_FREEZE_ACCOUNT:
    case IDX_TOKEN_IX_THAW_ACCOUNT:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->freeze_account.account,
                                err));
        IDX_TRY(require_account(tx, ix, 1, kind, &out->freeze_account.mint,
                                err));
        return require_account(tx, ix, 2, kind, &out->freeze_account.authority,
                               err);

    case IDX_TOKEN_IX_SYNC_NATIVE:
    case IDX_TOKEN_IX_INITIALIZE_IMMUTABLE_OWNER:
        return require_account(tx, ix, 0, kind, &out->account_only.account,
                               err);

    case IDX_TOKEN_IX_GET_ACCOUNT_DATA_SIZE:
        /* Token-2022 appends the extension types to weigh; the mint is all
         * this indexer takes from either program's form. */
        return require_account(tx, ix, 0, kind, &out->mint_query.mint, err);

    case IDX_TOKEN_IX_AMOUNT_TO_UI_AMOUNT:
        IDX_TRY(require_account(tx, ix, 0, kind, &out->mint_query.mint, err));
        return idx_cursor_u64le(&cursor, &out->mint_query.amount, err);

    case IDX_TOKEN_IX_UI_AMOUNT_TO_AMOUNT:
        IDX_TRY(require_account(tx, ix, 0, kind,
                                &out->ui_amount_to_amount.mint, err));
        /* The amount is the rest of the data, a decimal string. */
        return idx_cursor_take(&cursor, idx_cursor_remaining(&cursor),
                               &out->ui_amount_to_amount.ui_amount, err);
    }

    return IDX_FAIL(err, IDX_ERR_INTERNAL, "unhandled token instruction %u",
                    (unsigned)discriminant);
}
