#include "token_2022.h"

#include <string.h>

const char *idx_token_2022_ix_kind_name(idx_token_2022_ix_kind kind) {
    switch (kind) {
    case IDX_TOKEN_2022_IX_INITIALIZE_MINT_CLOSE_AUTHORITY:
        return "InitializeMintCloseAuthority";
    case IDX_TOKEN_2022_IX_TRANSFER_FEE_EXTENSION:
        return "TransferFeeExtension";
    case IDX_TOKEN_2022_IX_CONFIDENTIAL_TRANSFER_EXTENSION:
        return "ConfidentialTransferExtension";
    case IDX_TOKEN_2022_IX_DEFAULT_ACCOUNT_STATE_EXTENSION:
        return "DefaultAccountStateExtension";
    case IDX_TOKEN_2022_IX_REALLOCATE:
        return "Reallocate";
    case IDX_TOKEN_2022_IX_MEMO_TRANSFER_EXTENSION:
        return "MemoTransferExtension";
    case IDX_TOKEN_2022_IX_CREATE_NATIVE_MINT:
        return "CreateNativeMint";
    case IDX_TOKEN_2022_IX_INITIALIZE_NON_TRANSFERABLE_MINT:
        return "InitializeNonTransferableMint";
    case IDX_TOKEN_2022_IX_INTEREST_BEARING_MINT_EXTENSION:
        return "InterestBearingMintExtension";
    case IDX_TOKEN_2022_IX_CPI_GUARD_EXTENSION:
        return "CpiGuardExtension";
    case IDX_TOKEN_2022_IX_INITIALIZE_PERMANENT_DELEGATE:
        return "InitializePermanentDelegate";
    case IDX_TOKEN_2022_IX_TRANSFER_HOOK_EXTENSION:
        return "TransferHookExtension";
    case IDX_TOKEN_2022_IX_CONFIDENTIAL_TRANSFER_FEE_EXTENSION:
        return "ConfidentialTransferFeeExtension";
    case IDX_TOKEN_2022_IX_WITHDRAW_EXCESS_LAMPORTS:
        return "WithdrawExcessLamports";
    case IDX_TOKEN_2022_IX_METADATA_POINTER_EXTENSION:
        return "MetadataPointerExtension";
    case IDX_TOKEN_2022_IX_GROUP_POINTER_EXTENSION:
        return "GroupPointerExtension";
    case IDX_TOKEN_2022_IX_GROUP_MEMBER_POINTER_EXTENSION:
        return "GroupMemberPointerExtension";
    case IDX_TOKEN_2022_IX_CONFIDENTIAL_MINT_BURN_EXTENSION:
        return "ConfidentialMintBurnExtension";
    case IDX_TOKEN_2022_IX_SCALED_UI_AMOUNT_EXTENSION:
        return "ScaledUiAmountExtension";
    case IDX_TOKEN_2022_IX_PAUSABLE_EXTENSION:
        return "PausableExtension";
    }
    return "unknown";
}

bool idx_token_2022_ix_is_extension(idx_token_2022_ix_kind kind) {
    switch (kind) {
    case IDX_TOKEN_2022_IX_TRANSFER_FEE_EXTENSION:
    case IDX_TOKEN_2022_IX_CONFIDENTIAL_TRANSFER_EXTENSION:
    case IDX_TOKEN_2022_IX_DEFAULT_ACCOUNT_STATE_EXTENSION:
    case IDX_TOKEN_2022_IX_MEMO_TRANSFER_EXTENSION:
    case IDX_TOKEN_2022_IX_INTEREST_BEARING_MINT_EXTENSION:
    case IDX_TOKEN_2022_IX_CPI_GUARD_EXTENSION:
    case IDX_TOKEN_2022_IX_TRANSFER_HOOK_EXTENSION:
    case IDX_TOKEN_2022_IX_CONFIDENTIAL_TRANSFER_FEE_EXTENSION:
    case IDX_TOKEN_2022_IX_METADATA_POINTER_EXTENSION:
    case IDX_TOKEN_2022_IX_GROUP_POINTER_EXTENSION:
    case IDX_TOKEN_2022_IX_GROUP_MEMBER_POINTER_EXTENSION:
    case IDX_TOKEN_2022_IX_CONFIDENTIAL_MINT_BURN_EXTENSION:
    case IDX_TOKEN_2022_IX_SCALED_UI_AMOUNT_EXTENSION:
    case IDX_TOKEN_2022_IX_PAUSABLE_EXTENSION:
        return true;
    case IDX_TOKEN_2022_IX_INITIALIZE_MINT_CLOSE_AUTHORITY:
    case IDX_TOKEN_2022_IX_REALLOCATE:
    case IDX_TOKEN_2022_IX_CREATE_NATIVE_MINT:
    case IDX_TOKEN_2022_IX_INITIALIZE_NON_TRANSFERABLE_MINT:
    case IDX_TOKEN_2022_IX_INITIALIZE_PERMANENT_DELEGATE:
    case IDX_TOKEN_2022_IX_WITHDRAW_EXCESS_LAMPORTS:
        return false;
    }
    return false;
}

uint16_t idx_token_2022_extension_type_at(idx_slice extension_types,
                                          size_t index) {
    if (extension_types.data == NULL || index >= extension_types.len / 2) {
        return 0;
    }
    size_t offset = index * 2;
    return (uint16_t)((uint16_t)extension_types.data[offset] |
                      (uint16_t)(extension_types.data[offset + 1] << 8));
}

/* Resolves a named operand, failing with the variant's name when the
 * instruction is short of accounts. */
static idx_status require_account(const idx_transaction *tx,
                                  const idx_instruction *ix, size_t position,
                                  idx_token_2022_ix_kind kind,
                                  const idx_pubkey **out, idx_error *err) {
    const idx_pubkey *account = idx_instruction_account(tx, ix, position);
    if (account == NULL) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "token-2022 %s needs account %zu, instruction has %zu",
                        idx_token_2022_ix_kind_name(kind), position,
                        ix->account_count);
    }
    *out = account;
    return IDX_OK;
}

/* An extension group: the sub-discriminant, then a payload left as bytes for
 * whoever decodes that extension (decision D5). */
static idx_status decode_extension(idx_cursor *cursor,
                                   idx_token_2022_instruction *out,
                                   idx_error *err) {
    IDX_TRY(idx_cursor_u8(cursor, &out->extension.sub_discriminant, err));
    return idx_cursor_take(cursor, idx_cursor_remaining(cursor),
                           &out->extension.payload, err);
}

/* Reallocate names the extension types to grow the account for as a bare
 * sequence of u16 values: no length prefix, so it runs to the end. */
static idx_status decode_reallocate(const idx_transaction *tx,
                                    const idx_instruction *ix,
                                    idx_cursor *cursor,
                                    idx_token_2022_instruction *out,
                                    idx_error *err) {
    idx_token_2022_ix_kind kind = IDX_TOKEN_2022_IX_REALLOCATE;
    IDX_TRY(require_account(tx, ix, 0, kind, &out->reallocate.account, err));
    IDX_TRY(require_account(tx, ix, 1, kind, &out->reallocate.payer, err));
    /* Account 2 is the system program. */
    IDX_TRY(require_account(tx, ix, 3, kind, &out->reallocate.owner, err));

    idx_slice types = idx_slice_make(NULL, 0);
    IDX_TRY(idx_cursor_take(cursor, idx_cursor_remaining(cursor), &types,
                            err));
    if (types.len % 2 != 0) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "reallocate extension types are %zu bytes, not a whole "
                        "number of 16-bit types",
                        types.len);
    }
    out->reallocate.extension_types = types;
    out->reallocate.extension_type_count = types.len / 2;
    return IDX_OK;
}

idx_status idx_token_2022_instruction_decode(const idx_transaction *tx,
                                             const idx_instruction *ix,
                                             idx_token_2022_instruction *out,
                                             idx_error *err) {
    if (tx == NULL || ix == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, ix and out must not be NULL");
    }

    memset(out, 0, sizeof(*out));

    /* The shared range first. Only a discriminant past it comes back as
     * not-found, so anything else is this instruction's own failure. */
    idx_status status = idx_token_instruction_decode(tx, ix, &out->base, err);
    if (status == IDX_OK) {
        out->is_base = true;
        return IDX_OK;
    }
    if (status != IDX_ERR_NOT_FOUND) {
        return status;
    }

    idx_cursor cursor;
    idx_cursor_init(&cursor, ix->data);
    uint8_t discriminant = 0;
    IDX_TRY(idx_cursor_u8(&cursor, &discriminant, err));
    if (discriminant > (uint8_t)IDX_TOKEN_2022_IX_PAUSABLE_EXTENSION) {
        return IDX_FAIL(err, IDX_ERR_NOT_FOUND,
                        "unknown token-2022 instruction %u",
                        (unsigned)discriminant);
    }

    idx_token_2022_ix_kind kind = (idx_token_2022_ix_kind)discriminant;
    out->kind = kind;
    if (idx_token_2022_ix_is_extension(kind)) {
        return decode_extension(&cursor, out, err);
    }

    switch (kind) {
    case IDX_TOKEN_2022_IX_INITIALIZE_MINT_CLOSE_AUTHORITY: {
        const idx_pubkey **mint =
            &out->initialize_mint_close_authority.mint;
        IDX_TRY(require_account(tx, ix, 0, kind, mint, err));
        uint8_t tag = 0;
        IDX_TRY(idx_cursor_u8(&cursor, &tag, err));
        if (tag == 0) {
            return IDX_OK;
        }
        if (tag != 1) {
            return IDX_FAIL(err, IDX_ERR_PARSE,
                            "optional pubkey tag %u is neither 0 nor 1",
                            (unsigned)tag);
        }
        IDX_TRY(idx_cursor_copy(
            &cursor, out->initialize_mint_close_authority.close_authority.bytes,
            IDX_PUBKEY_LEN, err));
        out->initialize_mint_close_authority.has_close_authority = true;
        return IDX_OK;
    }

    case IDX_TOKEN_2022_IX_INITIALIZE_PERMANENT_DELEGATE:
        IDX_TRY(require_account(tx, ix, 0, kind,
                                &out->initialize_permanent_delegate.mint,
                                err));
        return idx_cursor_copy(
            &cursor, out->initialize_permanent_delegate.delegate.bytes,
            IDX_PUBKEY_LEN, err);

    case IDX_TOKEN_2022_IX_INITIALIZE_NON_TRANSFERABLE_MINT:
        return require_account(tx, ix, 0, kind, &out->mint_only.mint, err);

    case IDX_TOKEN_2022_IX_CREATE_NATIVE_MINT:
        /* The payer comes first here, and the mint it creates second. */
        IDX_TRY(require_account(tx, ix, 0, kind, &out->mint_only.payer, err));
        return require_account(tx, ix, 1, kind, &out->mint_only.mint, err);

    case IDX_TOKEN_2022_IX_WITHDRAW_EXCESS_LAMPORTS:
        IDX_TRY(require_account(tx, ix, 0, kind,
                                &out->withdraw_excess_lamports.source, err));
        IDX_TRY(require_account(tx, ix, 1, kind,
                                &out->withdraw_excess_lamports.destination,
                                err));
        return require_account(tx, ix, 2, kind,
                               &out->withdraw_excess_lamports.authority, err);

    case IDX_TOKEN_2022_IX_REALLOCATE:
        return decode_reallocate(tx, ix, &cursor, out, err);

    case IDX_TOKEN_2022_IX_TRANSFER_FEE_EXTENSION:
    case IDX_TOKEN_2022_IX_CONFIDENTIAL_TRANSFER_EXTENSION:
    case IDX_TOKEN_2022_IX_DEFAULT_ACCOUNT_STATE_EXTENSION:
    case IDX_TOKEN_2022_IX_MEMO_TRANSFER_EXTENSION:
    case IDX_TOKEN_2022_IX_INTEREST_BEARING_MINT_EXTENSION:
    case IDX_TOKEN_2022_IX_CPI_GUARD_EXTENSION:
    case IDX_TOKEN_2022_IX_TRANSFER_HOOK_EXTENSION:
    case IDX_TOKEN_2022_IX_CONFIDENTIAL_TRANSFER_FEE_EXTENSION:
    case IDX_TOKEN_2022_IX_METADATA_POINTER_EXTENSION:
    case IDX_TOKEN_2022_IX_GROUP_POINTER_EXTENSION:
    case IDX_TOKEN_2022_IX_GROUP_MEMBER_POINTER_EXTENSION:
    case IDX_TOKEN_2022_IX_CONFIDENTIAL_MINT_BURN_EXTENSION:
    case IDX_TOKEN_2022_IX_SCALED_UI_AMOUNT_EXTENSION:
    case IDX_TOKEN_2022_IX_PAUSABLE_EXTENSION:
        /* Handled above; listed so a new group cannot be added silently. */
        break;
    }

    return IDX_FAIL(err, IDX_ERR_INTERNAL,
                    "unhandled token-2022 instruction %u",
                    (unsigned)discriminant);
}
