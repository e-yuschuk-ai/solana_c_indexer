#include "block.h"

#include <string.h>

#include "base58.h"

const char *idx_tx_version_name(idx_tx_version version) {
    switch (version) {
    case IDX_TX_VERSION_LEGACY:
        return "legacy";
    case IDX_TX_VERSION_0:
        return "v0";
    }
    return "unknown";
}

const idx_pubkey *idx_instruction_program_id(const idx_transaction *tx,
                                             const idx_instruction *ix) {
    return &tx->accounts[ix->program_id_index].pubkey;
}

const idx_pubkey *idx_instruction_account(const idx_transaction *tx,
                                          const idx_instruction *ix,
                                          size_t position) {
    if (position >= ix->account_count) {
        return NULL;
    }
    return &tx->accounts[ix->account_indices[position]].pubkey;
}

/* Reads an object field that must fit in a byte (the header counts do). */
static idx_status read_u8_field(idx_json_val object, const char *key,
                                uint8_t *out, idx_error *err) {
    uint64_t value = 0;
    IDX_TRY(idx_json_read_u64(object, key, &value, err));
    if (value > UINT8_MAX) {
        return IDX_FAIL(err, IDX_ERR_PARSE, "%s = %llu does not fit in a byte",
                        key, (unsigned long long)value);
    }
    *out = (uint8_t)value;
    return IDX_OK;
}

/* Decodes a base58 pubkey held in an array element (a value in hand). */
static idx_status decode_pubkey_value(idx_json_val value, const char *what,
                                      idx_pubkey *out, idx_error *err) {
    idx_slice text;
    IDX_TRY(idx_json_as_string(value, what, &text, err));
    return idx_pubkey_from_base58((const char *)text.data, text.len, out, err);
}

/* Reads a base58 string field into a fixed-size value via `decode`. */
static idx_status read_hash_field(idx_json_val object, const char *key,
                                  idx_hash *out, idx_error *err) {
    idx_slice text;
    IDX_TRY(idx_json_read_string(object, key, &text, err));
    return idx_hash_from_base58((const char *)text.data, text.len, out, err);
}

/* Reads a base58 pubkey string field. */
static idx_status read_pubkey_field(idx_json_val object, const char *key,
                                    idx_pubkey *out, idx_error *err) {
    idx_slice text;
    IDX_TRY(idx_json_read_string(object, key, &text, err));
    return idx_pubkey_from_base58((const char *)text.data, text.len, out, err);
}

/*
 * Parses a decimal integer string into a u64. Token amounts arrive as strings
 * because they can reach the top of the u64 range, which a JSON number is not
 * guaranteed to carry exactly. Digits only — no sign, space or prefix — and an
 * overflow is a parse error rather than a silent wrap.
 */
static idx_status parse_u64_decimal(idx_slice text, uint64_t *out,
                                    idx_error *err) {
    if (text.len == 0) {
        return IDX_FAIL(err, IDX_ERR_PARSE, "empty numeric string");
    }
    uint64_t value = 0;
    for (size_t i = 0; i < text.len; i++) {
        char c = (char)text.data[i];
        if (c < '0' || c > '9') {
            return IDX_FAIL(err, IDX_ERR_PARSE,
                            "non-digit '%c' in numeric string", c);
        }
        uint64_t digit = (uint64_t)(c - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return IDX_FAIL(err, IDX_ERR_PARSE,
                            "numeric string overflows a 64-bit integer");
        }
        value = value * 10 + digit;
    }
    *out = value;
    return IDX_OK;
}

/*
 * Builds the resolved account list: the static message keys, then the loaded
 * writable addresses, then the loaded readonly ones (decision D7). The header
 * partitions the static keys into signer/writable classes; loaded addresses
 * are never signers and carry their writability by which list they came from.
 */
static idx_status resolve_accounts(idx_json_val message, idx_json_val meta,
                                   idx_tx_version version, idx_arena *arena,
                                   idx_transaction *tx, idx_error *err) {
    idx_json_val static_keys;
    IDX_TRY(idx_json_read_array(message, "accountKeys", &static_keys, err));
    size_t static_count = idx_json_array_size(static_keys);

    /* Loaded addresses live in meta, and only a versioned transaction has
     * them. A legacy one references its static keys alone. */
    idx_json_val loaded_writable = {NULL};
    idx_json_val loaded_readonly = {NULL};
    size_t writable_count = 0;
    size_t readonly_count = 0;
    if (version != IDX_TX_VERSION_LEGACY && idx_json_is_object(meta)) {
        idx_json_val loaded = idx_json_get(meta, "loadedAddresses");
        if (idx_json_is_object(loaded)) {
            loaded_writable = idx_json_get(loaded, "writable");
            loaded_readonly = idx_json_get(loaded, "readonly");
            writable_count = idx_json_array_size(loaded_writable);
            readonly_count = idx_json_array_size(loaded_readonly);
        }
    }

    size_t total = static_count + writable_count + readonly_count;
    idx_account *accounts = NULL;
    if (total > 0) {
        void *raw = NULL;
        IDX_TRY(idx_arena_calloc(arena, total, sizeof(*accounts), &raw, err));
        accounts = raw;
    }

    /* Header counts, validated so the writable/signer split below cannot
     * underflow on malformed input. */
    uint8_t sigs = tx->num_required_signatures;
    uint8_t ro_signed = tx->num_readonly_signed;
    uint8_t ro_unsigned = tx->num_readonly_unsigned;
    if ((size_t)sigs > static_count || ro_signed > sigs ||
        (size_t)ro_unsigned > static_count - sigs) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "message header counts exceed the %zu static keys",
                        static_count);
    }
    size_t writable_signed_end = (size_t)(sigs - ro_signed);
    size_t writable_unsigned_end = static_count - ro_unsigned;

    for (size_t i = 0; i < static_count; i++) {
        idx_account *account = &accounts[i];
        IDX_TRY(decode_pubkey_value(idx_json_array_get(static_keys, i),
                                    "accountKeys entry", &account->pubkey, err));
        bool is_signer = i < (size_t)sigs;
        account->is_signer = is_signer;
        account->is_writable = is_signer ? (i < writable_signed_end)
                                         : (i < writable_unsigned_end);
        account->from_lookup_table = false;
    }

    for (size_t i = 0; i < writable_count; i++) {
        idx_account *account = &accounts[static_count + i];
        IDX_TRY(decode_pubkey_value(idx_json_array_get(loaded_writable, i),
                                    "loaded writable entry", &account->pubkey,
                                    err));
        account->is_signer = false;
        account->is_writable = true;
        account->from_lookup_table = true;
    }

    for (size_t i = 0; i < readonly_count; i++) {
        idx_account *account =
            &accounts[static_count + writable_count + i];
        IDX_TRY(decode_pubkey_value(idx_json_array_get(loaded_readonly, i),
                                    "loaded readonly entry", &account->pubkey,
                                    err));
        account->is_signer = false;
        account->is_writable = false;
        account->from_lookup_table = true;
    }

    tx->accounts = accounts;
    tx->account_count = total;
    tx->static_account_count = static_count;
    return IDX_OK;
}

/* Validates an account index and narrows it to the byte the model stores. */
static idx_status take_index(idx_json_val value, size_t account_count,
                             uint8_t *out, idx_error *err) {
    uint64_t index = 0;
    IDX_TRY(idx_json_as_u64(value, "account index", &index, err));
    if (index >= account_count) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "account index %llu is past the %zu accounts",
                        (unsigned long long)index, account_count);
    }
    *out = (uint8_t)index;
    return IDX_OK;
}

/* Decodes one compiled instruction from `source` into `out`. */
static idx_status decode_instruction(idx_json_val source, size_t account_count,
                                     idx_arena *arena, idx_instruction *out,
                                     idx_error *err) {
    uint64_t program_index = 0;
    IDX_TRY(idx_json_read_u64(source, "programIdIndex", &program_index, err));
    if (program_index >= account_count) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "programIdIndex %llu is past the %zu accounts",
                        (unsigned long long)program_index, account_count);
    }
    out->program_id_index = (uint8_t)program_index;

    idx_json_val accounts;
    IDX_TRY(idx_json_read_array(source, "accounts", &accounts, err));
    size_t count = idx_json_array_size(accounts);
    uint8_t *indices = NULL;
    if (count > 0) {
        void *raw = NULL;
        IDX_TRY(idx_arena_calloc(arena, count, sizeof(*indices), &raw, err));
        indices = raw;
        for (size_t i = 0; i < count; i++) {
            IDX_TRY(take_index(idx_json_array_get(accounts, i), account_count,
                               &indices[i], err));
        }
    }
    out->account_indices = indices;
    out->account_count = count;

    idx_slice data_text;
    IDX_TRY(idx_json_read_string(source, "data", &data_text, err));
    if (data_text.len == 0) {
        out->data = idx_slice_make(NULL, 0);
    } else {
        size_t capacity = idx_base58_decoded_max(data_text.len);
        void *raw = NULL;
        IDX_TRY(idx_arena_alloc(arena, capacity, &raw, err));
        size_t decoded_len = 0;
        IDX_TRY(idx_base58_decode((const char *)data_text.data, data_text.len,
                                  raw, capacity, &decoded_len, err));
        out->data = idx_slice_make(raw, decoded_len);
    }

    uint64_t stack_height = 0;
    if (idx_json_opt_u64(source, "stackHeight", &stack_height) &&
        stack_height <= UINT16_MAX) {
        out->stack_height = (uint16_t)stack_height;
    } else {
        out->stack_height = 0;
    }
    return IDX_OK;
}

/* Decodes an array of compiled instructions (top-level or inner). */
static idx_status decode_instructions(idx_json_val array, size_t account_count,
                                      idx_arena *arena,
                                      const idx_instruction **out_list,
                                      size_t *out_count, idx_error *err) {
    size_t count = idx_json_array_size(array);
    idx_instruction *list = NULL;
    if (count > 0) {
        void *raw = NULL;
        IDX_TRY(idx_arena_calloc(arena, count, sizeof(*list), &raw, err));
        list = raw;
        for (size_t i = 0; i < count; i++) {
            IDX_TRY(decode_instruction(idx_json_array_get(array, i),
                                       account_count, arena, &list[i], err));
        }
    }
    *out_list = list;
    *out_count = count;
    return IDX_OK;
}

/* Decodes meta.innerInstructions, whose entries name the top-level index they
 * expand and carry their own instruction list. Absent when the block was
 * fetched without full detail, which is not an error here. */
static idx_status decode_inner_instructions(idx_json_val meta,
                                            size_t account_count,
                                            idx_arena *arena,
                                            idx_transaction *tx,
                                            idx_error *err) {
    tx->inner_instructions = NULL;
    tx->inner_instruction_count = 0;
    if (!idx_json_is_object(meta)) {
        return IDX_OK;
    }
    idx_json_val groups = idx_json_get(meta, "innerInstructions");
    if (!idx_json_is_array(groups)) {
        return IDX_OK;
    }
    size_t count = idx_json_array_size(groups);
    if (count == 0) {
        return IDX_OK;
    }

    void *raw = NULL;
    IDX_TRY(idx_arena_calloc(arena, count, sizeof(idx_inner_instructions), &raw,
                             err));
    idx_inner_instructions *list = raw;
    for (size_t i = 0; i < count; i++) {
        idx_json_val group = idx_json_array_get(groups, i);
        uint8_t index = 0;
        IDX_TRY(read_u8_field(group, "index", &index, err));
        list[i].index = index;

        idx_json_val instructions;
        IDX_TRY(idx_json_read_array(group, "instructions", &instructions, err));
        IDX_TRY(decode_instructions(instructions, account_count, arena,
                                    &list[i].instructions,
                                    &list[i].instruction_count, err));
    }
    tx->inner_instructions = list;
    tx->inner_instruction_count = count;
    return IDX_OK;
}

/* Reads the message version: absent or "legacy" is legacy, the number 0 is v0,
 * anything else is a version this decoder does not model. */
static idx_status read_version(idx_json_val entry, idx_tx_version *out,
                               idx_error *err) {
    idx_json_val value = idx_json_get(entry, "version");
    if (!idx_json_is_present(value)) {
        *out = IDX_TX_VERSION_LEGACY;
        return IDX_OK;
    }
    if (idx_json_is_string(value)) {
        idx_slice text;
        IDX_TRY(idx_json_as_string(value, "version", &text, err));
        if (idx_slice_equal(text, idx_slice_from_str("legacy"))) {
            *out = IDX_TX_VERSION_LEGACY;
            return IDX_OK;
        }
        return IDX_FAIL(err, IDX_ERR_PARSE, "unknown transaction version '%.*s'",
                        (int)text.len, (const char *)text.data);
    }
    uint64_t number = 0;
    IDX_TRY(idx_json_as_u64(value, "version", &number, err));
    if (number != 0) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "unsupported transaction version %llu",
                        (unsigned long long)number);
    }
    *out = IDX_TX_VERSION_0;
    return IDX_OK;
}

static idx_status decode_signatures(idx_json_val transaction, idx_arena *arena,
                                    idx_transaction *tx, idx_error *err) {
    idx_json_val signatures;
    IDX_TRY(idx_json_read_array(transaction, "signatures", &signatures, err));
    size_t count = idx_json_array_size(signatures);
    idx_signature *list = NULL;
    if (count > 0) {
        void *raw = NULL;
        IDX_TRY(idx_arena_calloc(arena, count, sizeof(*list), &raw, err));
        list = raw;
        for (size_t i = 0; i < count; i++) {
            idx_slice text;
            IDX_TRY(idx_json_as_string(idx_json_array_get(signatures, i),
                                       "signature", &text, err));
            IDX_TRY(idx_signature_from_base58((const char *)text.data, text.len,
                                              &list[i], err));
        }
    }
    tx->signatures = list;
    tx->signature_count = count;
    return IDX_OK;
}

/* Pulls the small, structural part of meta: whether the transaction succeeded
 * and the fee it paid. */
static idx_status decode_meta_summary(idx_json_val meta, idx_transaction *tx,
                                      idx_error *err) {
    tx->success = true;
    tx->fee = 0;
    if (!idx_json_is_object(meta)) {
        return IDX_OK;
    }
    idx_json_val error = idx_json_get(meta, "err");
    tx->success = !idx_json_is_present(error) || idx_json_is_null(error);
    (void)idx_json_opt_u64(meta, "fee", &tx->fee);
    (void)err;
    return IDX_OK;
}

/*
 * Reads meta.pre/postBalances: two parallel lamport arrays, one entry per
 * resolved account. They come as a pair, so either both are decoded or neither
 * is; a length that disagrees with the account list is malformed.
 */
static idx_status decode_balances(idx_json_val meta, idx_arena *arena,
                                  idx_transaction *tx, idx_error *err) {
    tx->pre_balances = NULL;
    tx->post_balances = NULL;
    tx->balance_count = 0;
    if (!idx_json_is_object(meta)) {
        return IDX_OK;
    }

    idx_json_val pre = idx_json_get(meta, "preBalances");
    idx_json_val post = idx_json_get(meta, "postBalances");
    bool has_pre = idx_json_is_array(pre);
    bool has_post = idx_json_is_array(post);
    if (!has_pre && !has_post) {
        return IDX_OK;
    }
    if (!has_pre || !has_post) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "preBalances and postBalances must both be present");
    }

    size_t count = idx_json_array_size(pre);
    if (idx_json_array_size(post) != count) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "preBalances and postBalances differ in length");
    }
    if (count != tx->account_count) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "%zu balances for %zu accounts", count,
                        tx->account_count);
    }
    if (count == 0) {
        return IDX_OK;
    }

    void *pre_raw = NULL;
    void *post_raw = NULL;
    IDX_TRY(idx_arena_calloc(arena, count, sizeof(uint64_t), &pre_raw, err));
    IDX_TRY(idx_arena_calloc(arena, count, sizeof(uint64_t), &post_raw, err));
    uint64_t *pre_values = pre_raw;
    uint64_t *post_values = post_raw;
    for (size_t i = 0; i < count; i++) {
        IDX_TRY(idx_json_as_u64(idx_json_array_get(pre, i), "preBalance",
                                &pre_values[i], err));
        IDX_TRY(idx_json_as_u64(idx_json_array_get(post, i), "postBalance",
                                &post_values[i], err));
    }

    tx->pre_balances = pre_values;
    tx->post_balances = post_values;
    tx->balance_count = count;
    return IDX_OK;
}

/* Decodes one pre/postTokenBalances entry. */
static idx_status decode_token_balance(idx_json_val entry, size_t account_count,
                                       idx_token_balance *out, idx_error *err) {
    memset(out, 0, sizeof(*out));

    uint64_t index = 0;
    IDX_TRY(idx_json_read_u64(entry, "accountIndex", &index, err));
    if (index >= account_count) {
        return IDX_FAIL(err, IDX_ERR_PARSE,
                        "token balance accountIndex %llu is past the %zu "
                        "accounts",
                        (unsigned long long)index, account_count);
    }
    out->account_index = (uint8_t)index;

    IDX_TRY(read_pubkey_field(entry, "mint", &out->mint, err));

    idx_json_val owner = idx_json_get(entry, "owner");
    if (idx_json_is_string(owner)) {
        IDX_TRY(decode_pubkey_value(owner, "token balance owner", &out->owner,
                                    err));
        out->has_owner = true;
    }
    idx_json_val program_id = idx_json_get(entry, "programId");
    if (idx_json_is_string(program_id)) {
        IDX_TRY(decode_pubkey_value(program_id, "token balance programId",
                                    &out->program_id, err));
        out->has_program_id = true;
    }

    idx_json_val ui;
    IDX_TRY(idx_json_read_object(entry, "uiTokenAmount", &ui, err));
    idx_slice amount_text;
    IDX_TRY(idx_json_read_string(ui, "amount", &amount_text, err));
    IDX_TRY(parse_u64_decimal(amount_text, &out->amount, err));
    uint64_t decimals = 0;
    IDX_TRY(idx_json_read_u64(ui, "decimals", &decimals, err));
    if (decimals > UINT8_MAX) {
        return IDX_FAIL(err, IDX_ERR_PARSE, "token decimals %llu does not fit "
                        "in a byte", (unsigned long long)decimals);
    }
    out->decimals = (uint8_t)decimals;
    return IDX_OK;
}

/* Decodes a pre/postTokenBalances array, absent or empty being no error. */
static idx_status decode_token_balances(idx_json_val meta, const char *key,
                                        size_t account_count, idx_arena *arena,
                                        const idx_token_balance **out_list,
                                        size_t *out_count, idx_error *err) {
    *out_list = NULL;
    *out_count = 0;
    if (!idx_json_is_object(meta)) {
        return IDX_OK;
    }
    idx_json_val array = idx_json_get(meta, key);
    if (!idx_json_is_array(array)) {
        return IDX_OK;
    }
    size_t count = idx_json_array_size(array);
    if (count == 0) {
        return IDX_OK;
    }

    void *raw = NULL;
    IDX_TRY(idx_arena_calloc(arena, count, sizeof(idx_token_balance), &raw,
                             err));
    idx_token_balance *list = raw;
    for (size_t i = 0; i < count; i++) {
        IDX_TRY(decode_token_balance(idx_json_array_get(array, i), account_count,
                                     &list[i], err));
    }
    *out_list = list;
    *out_count = count;
    return IDX_OK;
}

/* Decodes meta.logMessages: an array of strings, or null when the runtime
 * truncated them. Each log borrows the document. */
static idx_status decode_logs(idx_json_val meta, idx_arena *arena,
                              idx_transaction *tx, idx_error *err) {
    tx->logs = NULL;
    tx->log_count = 0;
    if (!idx_json_is_object(meta)) {
        return IDX_OK;
    }
    idx_json_val array = idx_json_get(meta, "logMessages");
    if (!idx_json_is_array(array)) {
        return IDX_OK;
    }
    size_t count = idx_json_array_size(array);
    if (count == 0) {
        return IDX_OK;
    }

    void *raw = NULL;
    IDX_TRY(idx_arena_calloc(arena, count, sizeof(idx_slice), &raw, err));
    idx_slice *list = raw;
    for (size_t i = 0; i < count; i++) {
        IDX_TRY(idx_json_as_string(idx_json_array_get(array, i), "log message",
                                   &list[i], err));
    }
    tx->logs = list;
    tx->log_count = count;
    return IDX_OK;
}

static idx_status decode_transaction(idx_json_val entry, idx_arena *arena,
                                     idx_transaction *tx, idx_error *err) {
    memset(tx, 0, sizeof(*tx));

    IDX_TRY(read_version(entry, &tx->version, err));

    idx_json_val transaction;
    IDX_TRY(idx_json_read_object(entry, "transaction", &transaction, err));
    idx_json_val message;
    IDX_TRY(idx_json_read_object(transaction, "message", &message, err));
    idx_json_val meta = idx_json_get(entry, "meta");

    IDX_TRY(decode_signatures(transaction, arena, tx, err));

    idx_json_val header;
    IDX_TRY(idx_json_read_object(message, "header", &header, err));
    IDX_TRY(read_u8_field(header, "numRequiredSignatures",
                          &tx->num_required_signatures, err));
    IDX_TRY(read_u8_field(header, "numReadonlySignedAccounts",
                          &tx->num_readonly_signed, err));
    IDX_TRY(read_u8_field(header, "numReadonlyUnsignedAccounts",
                          &tx->num_readonly_unsigned, err));
    IDX_TRY(read_hash_field(message, "recentBlockhash", &tx->recent_blockhash,
                            err));

    IDX_TRY(resolve_accounts(message, meta, tx->version, arena, tx, err));

    idx_json_val instructions;
    IDX_TRY(idx_json_read_array(message, "instructions", &instructions, err));
    IDX_TRY(decode_instructions(instructions, tx->account_count, arena,
                                &tx->instructions, &tx->instruction_count, err));

    IDX_TRY(decode_inner_instructions(meta, tx->account_count, arena, tx, err));
    IDX_TRY(decode_meta_summary(meta, tx, err));
    IDX_TRY(decode_balances(meta, arena, tx, err));
    IDX_TRY(decode_token_balances(meta, "preTokenBalances", tx->account_count,
                                  arena, &tx->pre_token_balances,
                                  &tx->pre_token_balance_count, err));
    IDX_TRY(decode_token_balances(meta, "postTokenBalances", tx->account_count,
                                  arena, &tx->post_token_balances,
                                  &tx->post_token_balance_count, err));
    IDX_TRY(decode_logs(meta, arena, tx, err));
    return IDX_OK;
}

idx_status idx_block_decode(idx_json_val block, idx_slot slot, idx_arena *arena,
                            idx_block *out, idx_error *err) {
    if (out == NULL || arena == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "out and arena must not be NULL");
    }
    if (!idx_json_is_object(block)) {
        return IDX_FAIL(err, IDX_ERR_PARSE, "block is not an object");
    }

    memset(out, 0, sizeof(*out));
    out->slot = slot;

    IDX_TRY(read_hash_field(block, "blockhash", &out->blockhash, err));
    IDX_TRY(idx_json_read_u64(block, "parentSlot", &out->parent_slot, err));

    idx_json_val previous = idx_json_get(block, "previousBlockhash");
    if (idx_json_is_string(previous)) {
        IDX_TRY(read_hash_field(block, "previousBlockhash",
                                &out->previous_blockhash, err));
    }

    uint64_t height = 0;
    if (idx_json_opt_u64(block, "blockHeight", &height)) {
        out->block_height = height;
        out->has_block_height = true;
    }
    idx_json_val block_time = idx_json_get(block, "blockTime");
    if (idx_json_is_present(block_time) && !idx_json_is_null(block_time)) {
        int64_t seconds = 0;
        IDX_TRY(idx_json_read_i64(block, "blockTime", &seconds, err));
        out->block_time = seconds;
        out->has_block_time = true;
    }

    idx_json_val transactions;
    IDX_TRY(idx_json_read_array(block, "transactions", &transactions, err));
    size_t count = idx_json_array_size(transactions);
    idx_transaction *list = NULL;
    if (count > 0) {
        void *raw = NULL;
        IDX_TRY(idx_arena_calloc(arena, count, sizeof(*list), &raw, err));
        list = raw;
        for (size_t i = 0; i < count; i++) {
            IDX_TRY(decode_transaction(idx_json_array_get(transactions, i),
                                       arena, &list[i], err));
        }
    }
    out->transactions = list;
    out->transaction_count = count;
    return IDX_OK;
}
