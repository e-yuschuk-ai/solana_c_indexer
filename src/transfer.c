#include "transfer.h"

#include <string.h>

#include "system_program.h"
#include "token_2022.h"
#include "token_program.h"

const char *idx_transfer_kind_name(idx_transfer_kind kind) {
    switch (kind) {
    case IDX_TRANSFER_SOL:
        return "sol";
    case IDX_TRANSFER_TOKEN:
        return "token";
    case IDX_TRANSFER_MINT:
        return "mint";
    case IDX_TRANSFER_BURN:
        return "burn";
    }
    return "unknown";
}

/*
 * The rows of one transaction, filled as the walk goes. `capacity` is one row
 * per instruction, which is the most any walk can produce, so `emit` cannot run
 * out; the check is there because a silent overrun is the one failure mode this
 * would not survive.
 */
typedef struct {
    const idx_transaction *tx;
    idx_transfer *rows;
    size_t count;
    size_t capacity;
    uint16_t instruction_index;
    uint16_t inner_index;
    bool inner;
} builder;

/*
 * The row to fill, zeroed and stamped with where the walk is, or NULL when
 * there is no room. A row is only counted once `commit` accepts it.
 */
static idx_transfer *builder_row(builder *b) {
    if (b->count >= b->capacity) {
        return NULL;
    }
    idx_transfer *row = &b->rows[b->count];
    memset(row, 0, sizeof(*row));
    row->instruction_index = b->instruction_index;
    row->inner_index = b->inner_index;
    row->inner = b->inner;
    return row;
}

/* The token balance `meta` carries for `account`, or NULL. The post side is
 * asked first because it is the transaction's own view of the account; the pre
 * side answers for one that was closed by it. */
static const idx_token_balance *find_token_balance(const idx_transaction *tx,
                                                   const idx_pubkey *account) {
    const idx_token_balance *lists[2] = {tx->post_token_balances,
                                         tx->pre_token_balances};
    size_t counts[2] = {tx->post_token_balance_count,
                        tx->pre_token_balance_count};
    for (size_t list = 0; list < 2; list++) {
        for (size_t i = 0; i < counts[list]; i++) {
            const idx_token_balance *entry = &lists[list][i];
            if (idx_pubkey_equal(&tx->accounts[entry->account_index].pubkey,
                                 account)) {
                return entry;
            }
        }
    }
    return NULL;
}

/*
 * Fills what the instruction did not say from the token balances of the same
 * accounts. An unchecked Transfer names neither the mint nor its scale, and a
 * token account never names its owner, so without this a row would say that
 * some amount of something moved between two accounts.
 */
static void resolve_from_balances(const idx_transaction *tx,
                                  idx_transfer *row) {
    /* A mint is one end of these, and it is not a token account, so only the
     * end that holds tokens is looked up. */
    const idx_token_balance *source =
        (row->kind != IDX_TRANSFER_MINT)
            ? find_token_balance(tx, &row->source)
            : NULL;
    const idx_token_balance *destination =
        (row->kind != IDX_TRANSFER_BURN)
            ? find_token_balance(tx, &row->destination)
            : NULL;

    const idx_token_balance *either = (source != NULL) ? source : destination;
    if (either != NULL) {
        if (!row->has_mint) {
            row->mint = either->mint;
            row->has_mint = true;
        }
        if (!row->has_decimals) {
            row->decimals = either->decimals;
            row->has_decimals = true;
        }
    }
    if (source != NULL && source->has_owner) {
        row->source_owner = source->owner;
        row->has_source_owner = true;
    }
    if (destination != NULL && destination->has_owner) {
        row->destination_owner = destination->owner;
        row->has_destination_owner = true;
    }
}

/* Accepts a filled row, unless it moves nothing. */
static void builder_commit(builder *b, idx_transfer *row) {
    if (row->amount == 0) {
        return;
    }
    if (row->kind != IDX_TRANSFER_SOL) {
        resolve_from_balances(b->tx, row);
    }
    b->count++;
}

/* ------------------------------------------------------------- system -- */

static idx_status extract_system(builder *b, const idx_instruction *ix,
                                 idx_error *err) {
    idx_system_instruction decoded;
    idx_status status = idx_system_instruction_decode(b->tx, ix, &decoded, err);
    if (status == IDX_ERR_NOT_FOUND) {
        return IDX_OK; /* a program upgrade this decoder has not seen */
    }
    if (status != IDX_OK) {
        return status;
    }

    idx_transfer *row = builder_row(b);
    if (row == NULL) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL,
                        "more transfers than instructions to hold them");
    }
    row->kind = IDX_TRANSFER_SOL;

    switch (decoded.kind) {
    case IDX_SYSTEM_IX_TRANSFER:
        row->source = *decoded.transfer.from;
        row->destination = *decoded.transfer.to;
        row->amount = decoded.transfer.lamports;
        break;

    /* The source signs for itself in a plain transfer, but here a separate
     * account does, and which one is the whole point of the variant. */
    case IDX_SYSTEM_IX_TRANSFER_WITH_SEED:
        row->source = *decoded.transfer_with_seed.from;
        row->destination = *decoded.transfer_with_seed.to;
        row->authority = *decoded.transfer_with_seed.base_account;
        row->has_authority = true;
        row->amount = decoded.transfer_with_seed.lamports;
        break;

    case IDX_SYSTEM_IX_CREATE_ACCOUNT:
        row->source = *decoded.create_account.funder;
        row->destination = *decoded.create_account.account;
        row->amount = decoded.create_account.lamports;
        break;

    case IDX_SYSTEM_IX_CREATE_ACCOUNT_WITH_SEED:
        row->source = *decoded.create_account_with_seed.funder;
        row->destination = *decoded.create_account_with_seed.account;
        row->amount = decoded.create_account_with_seed.lamports;
        break;

    case IDX_SYSTEM_IX_WITHDRAW_NONCE_ACCOUNT:
        row->source = *decoded.withdraw_nonce.nonce;
        row->destination = *decoded.withdraw_nonce.to;
        row->authority = *decoded.withdraw_nonce.authority;
        row->has_authority = true;
        row->amount = decoded.withdraw_nonce.lamports;
        break;

    /* The rest move no lamports: they assign, allocate, or manage a nonce. */
    case IDX_SYSTEM_IX_ASSIGN:
    case IDX_SYSTEM_IX_ADVANCE_NONCE_ACCOUNT:
    case IDX_SYSTEM_IX_INITIALIZE_NONCE_ACCOUNT:
    case IDX_SYSTEM_IX_AUTHORIZE_NONCE_ACCOUNT:
    case IDX_SYSTEM_IX_ALLOCATE:
    case IDX_SYSTEM_IX_ALLOCATE_WITH_SEED:
    case IDX_SYSTEM_IX_ASSIGN_WITH_SEED:
    case IDX_SYSTEM_IX_UPGRADE_NONCE_ACCOUNT:
        return IDX_OK;
    }

    builder_commit(b, row);
    return IDX_OK;
}

/* -------------------------------------------------------------- token -- */

/* Fills a row from one of the base instructions both token programs share.
 * Returns false for an instruction that moves no tokens. */
static bool fill_from_token(const idx_token_instruction *decoded,
                            idx_transfer *row) {
    switch (decoded->kind) {
    case IDX_TOKEN_IX_TRANSFER:
    case IDX_TOKEN_IX_TRANSFER_CHECKED:
        row->kind = IDX_TRANSFER_TOKEN;
        row->source = *decoded->transfer.source;
        row->destination = *decoded->transfer.destination;
        row->authority = *decoded->transfer.authority;
        row->has_authority = true;
        row->amount = decoded->transfer.amount;
        if (decoded->transfer.mint != NULL) {
            row->mint = *decoded->transfer.mint;
            row->has_mint = true;
        }
        row->decimals = decoded->transfer.decimals;
        row->has_decimals = decoded->transfer.has_decimals;
        return true;

    /* Minted tokens come from the mint itself, which is the only end of the
     * movement that is not an account holding a balance. */
    case IDX_TOKEN_IX_MINT_TO:
    case IDX_TOKEN_IX_MINT_TO_CHECKED:
        row->kind = IDX_TRANSFER_MINT;
        row->source = *decoded->mint_to.mint;
        row->destination = *decoded->mint_to.account;
        row->authority = *decoded->mint_to.authority;
        row->has_authority = true;
        row->mint = *decoded->mint_to.mint;
        row->has_mint = true;
        row->amount = decoded->mint_to.amount;
        row->decimals = decoded->mint_to.decimals;
        row->has_decimals = decoded->mint_to.has_decimals;
        return true;

    case IDX_TOKEN_IX_BURN:
    case IDX_TOKEN_IX_BURN_CHECKED:
        row->kind = IDX_TRANSFER_BURN;
        row->source = *decoded->burn.account;
        row->destination = *decoded->burn.mint;
        row->authority = *decoded->burn.authority;
        row->has_authority = true;
        row->mint = *decoded->burn.mint;
        row->has_mint = true;
        row->amount = decoded->burn.amount;
        row->decimals = decoded->burn.decimals;
        row->has_decimals = decoded->burn.has_decimals;
        return true;

    /* Everything else authorizes, initializes or queries. A CloseAccount
     * moves lamports, but not an amount any instruction states. */
    case IDX_TOKEN_IX_INITIALIZE_MINT:
    case IDX_TOKEN_IX_INITIALIZE_ACCOUNT:
    case IDX_TOKEN_IX_INITIALIZE_MULTISIG:
    case IDX_TOKEN_IX_APPROVE:
    case IDX_TOKEN_IX_REVOKE:
    case IDX_TOKEN_IX_SET_AUTHORITY:
    case IDX_TOKEN_IX_CLOSE_ACCOUNT:
    case IDX_TOKEN_IX_FREEZE_ACCOUNT:
    case IDX_TOKEN_IX_THAW_ACCOUNT:
    case IDX_TOKEN_IX_APPROVE_CHECKED:
    case IDX_TOKEN_IX_INITIALIZE_ACCOUNT2:
    case IDX_TOKEN_IX_SYNC_NATIVE:
    case IDX_TOKEN_IX_INITIALIZE_ACCOUNT3:
    case IDX_TOKEN_IX_INITIALIZE_MULTISIG2:
    case IDX_TOKEN_IX_INITIALIZE_MINT2:
    case IDX_TOKEN_IX_GET_ACCOUNT_DATA_SIZE:
    case IDX_TOKEN_IX_INITIALIZE_IMMUTABLE_OWNER:
    case IDX_TOKEN_IX_AMOUNT_TO_UI_AMOUNT:
    case IDX_TOKEN_IX_UI_AMOUNT_TO_AMOUNT:
        break;
    }
    return false;
}

static idx_status extract_token(builder *b, const idx_instruction *ix,
                                idx_error *err) {
    idx_token_instruction decoded;
    idx_status status = idx_token_instruction_decode(b->tx, ix, &decoded, err);
    if (status == IDX_ERR_NOT_FOUND) {
        return IDX_OK;
    }
    if (status != IDX_OK) {
        return status;
    }

    idx_transfer *row = builder_row(b);
    if (row == NULL) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL,
                        "more transfers than instructions to hold them");
    }
    if (fill_from_token(&decoded, row)) {
        builder_commit(b, row);
    }
    return IDX_OK;
}

/*
 * Token-2022 adds one instruction that moves tokens: the transfer of a mint
 * that charges a fee, which goes through the transfer fee extension instead of
 * TransferChecked. Everything else it has beyond the base set either moves
 * nothing or moves it confidentially, where the amount is encrypted and there
 * is nothing to record.
 */
static idx_status extract_token_2022(builder *b, const idx_instruction *ix,
                                     idx_error *err) {
    idx_token_2022_instruction decoded;
    idx_status status =
        idx_token_2022_instruction_decode(b->tx, ix, &decoded, err);
    if (status == IDX_ERR_NOT_FOUND) {
        return IDX_OK;
    }
    if (status != IDX_OK) {
        return status;
    }

    idx_transfer *row = builder_row(b);
    if (row == NULL) {
        return IDX_FAIL(err, IDX_ERR_INTERNAL,
                        "more transfers than instructions to hold them");
    }

    if (decoded.is_base) {
        if (fill_from_token(&decoded.base, row)) {
            builder_commit(b, row);
        }
        return IDX_OK;
    }
    if (decoded.kind != IDX_TOKEN_2022_IX_TRANSFER_FEE_EXTENSION ||
        decoded.extension.sub_discriminant !=
            IDX_TOKEN_2022_FEE_IX_TRANSFER_CHECKED_WITH_FEE) {
        return IDX_OK;
    }

    idx_token_2022_transfer_with_fee transfer;
    IDX_TRY(idx_token_2022_transfer_with_fee_decode(
        b->tx, ix, decoded.extension.payload, &transfer, err));
    row->kind = IDX_TRANSFER_TOKEN;
    row->source = *transfer.source;
    row->destination = *transfer.destination;
    row->authority = *transfer.authority;
    row->has_authority = true;
    row->mint = *transfer.mint;
    row->has_mint = true;
    row->amount = transfer.amount;
    row->fee = transfer.fee;
    row->decimals = transfer.decimals;
    row->has_decimals = true;
    builder_commit(b, row);
    return IDX_OK;
}

/* --------------------------------------------------------------- walk -- */

static idx_status extract_instruction(builder *b, const idx_instruction *ix,
                                      idx_error *err) {
    const idx_pubkey *program = idx_instruction_program_id(b->tx, ix);
    if (idx_pubkey_equal(program, &IDX_PROGRAM_SYSTEM)) {
        return extract_system(b, ix, err);
    }
    if (idx_pubkey_equal(program, &IDX_PROGRAM_TOKEN)) {
        return extract_token(b, ix, err);
    }
    if (idx_pubkey_equal(program, &IDX_PROGRAM_TOKEN_2022)) {
        return extract_token_2022(b, ix, err);
    }
    return IDX_OK;
}

/* The inner instructions of top-level instruction `index`, or NULL. The groups
 * are few and arrive in order, so a scan is cheaper than anything that would
 * index them. */
static const idx_inner_instructions *inner_group(const idx_transaction *tx,
                                                 size_t index) {
    for (size_t i = 0; i < tx->inner_instruction_count; i++) {
        if (tx->inner_instructions[i].index == index) {
            return &tx->inner_instructions[i];
        }
    }
    return NULL;
}

/* One row per instruction is the ceiling, so this is what the arena is asked
 * for. */
static size_t instruction_total(const idx_transaction *tx) {
    size_t total = tx->instruction_count;
    for (size_t i = 0; i < tx->inner_instruction_count; i++) {
        total += tx->inner_instructions[i].instruction_count;
    }
    return total;
}

idx_status idx_transfer_extract(const idx_transaction *tx, idx_arena *arena,
                                const idx_transfer **out, size_t *out_count,
                                idx_error *err) {
    if (tx == NULL || arena == NULL || out == NULL || out_count == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "tx, arena, out and out_count must not be NULL");
    }

    *out = NULL;
    *out_count = 0;
    if (!tx->has_meta || !tx->success || tx->accounts == NULL) {
        return IDX_OK;
    }
    size_t capacity = instruction_total(tx);
    if (capacity == 0) {
        return IDX_OK;
    }

    void *raw = NULL;
    IDX_TRY(idx_arena_calloc(arena, capacity, sizeof(idx_transfer), &raw, err));

    builder b;
    memset(&b, 0, sizeof(b));
    b.tx = tx;
    b.rows = raw;
    b.capacity = capacity;

    for (size_t i = 0; i < tx->instruction_count; i++) {
        b.instruction_index = (uint16_t)i;
        b.inner_index = 0;
        b.inner = false;
        IDX_TRY(extract_instruction(&b, &tx->instructions[i], err));

        /* The CPIs the instruction made, which is where most token movement
         * is: a venue's program transfers on the trader's behalf. */
        const idx_inner_instructions *group = inner_group(tx, i);
        if (group == NULL) {
            continue;
        }
        b.inner = true;
        for (size_t j = 0; j < group->instruction_count; j++) {
            b.inner_index = (uint16_t)j;
            IDX_TRY(extract_instruction(&b, &group->instructions[j], err));
        }
    }

    if (b.count == 0) {
        return IDX_OK;
    }
    *out = b.rows;
    *out_count = b.count;
    return IDX_OK;
}
