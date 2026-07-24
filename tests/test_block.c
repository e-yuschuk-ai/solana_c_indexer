/*
 * Block decoding. The fixtures are hand-built but shaped exactly like the
 * `encoding: json` blocks the transport delivers: real base58 keys and
 * signatures, a legacy transaction and a versioned one whose accounts are
 * resolved through loaded addresses. Error paths cover the malformed shapes a
 * provider could hand us — a bad version, an out-of-range index, header counts
 * that do not fit the static keys.
 */
#include "block.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base58.h"
#include "test.h"

/* Real base58 values, so the decoders exercise their actual paths. */
#define SIG_A                                                                 \
    "2WjGQqLqhtRXMix7SubSkb8tVRgUxYJbDjhMHv7S4BmMF2WHLBKEX7t4mytSHVFDeupp"     \
    "uFubb7wAD7gRDBp9cZtg"
#define BLOCKHASH "HJJCYsevLqhvtuwUgL3g6LV7fhvSoECsHTHbduqD9Sjj"
#define RECENT "3dUvn19Fx7TdkqKtbYwDvQR8bbsmjQoGYSiUEcYth8NT"
#define PAYER "BLCAvcA71yTvonyAXAMmN97W6WtqpRDT4B76NgRqmBFH"
#define OTHER "CeTxXg1RTkzucvXY62jZKKyibk4y1GcnXBfmMS9odzQg"
#define SYSTEM_KEY "11111111111111111111111111111111"
#define TOKEN_KEY "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
#define LOADED_W "76sxKrPtgoJHDJvxwFHqb3cAXWfRHFLe3VpKcLCAHSEf"
#define LOADED_R "SysvarRecentB1ockHashes11111111111111111111"
#define ALT_KEY "4vX5U9XsiY11infmC13d6VFPjvUqtuRw744r4o94dyow"

/* base58 "Ldp" decodes to the three bytes {1, 2, 3}. */
#define DATA_123 "Ldp"

static idx_json_doc *parse(const char *text) {
    idx_json_doc *doc = NULL;
    idx_error err;
    idx_error_clear(&err);
    idx_status status = idx_json_parse(idx_slice_from_str(text), &doc, &err);
    TEST_CHECK(status == IDX_OK, "parse failed: %s", err.message);
    return doc;
}

/* Re-encodes decoded instruction bytes and compares to the base58 they came
 * from, which checks the decode without hard-coding the byte values here. */
static void check_data(idx_slice data, const char *expected) {
    char buffer[64];
    idx_error err;
    idx_error_clear(&err);
    idx_status status =
        idx_base58_encode(data, buffer, sizeof(buffer), NULL, &err);
    TEST_CHECK(status == IDX_OK, "encode failed: %s", err.message);
    TEST_EQ_STR(buffer, expected);
}

static const char *const LEGACY_BLOCK =
    "{\"blockhash\":\"" BLOCKHASH "\","
    " \"previousBlockhash\":\"" RECENT "\","
    " \"parentSlot\":434767209, \"blockHeight\":412827004,"
    " \"blockTime\":1784831561,"
    " \"transactions\":[{"
    "  \"version\":\"legacy\","
    "  \"transaction\":{\"signatures\":[\"" SIG_A "\"],"
    "   \"message\":{"
    "    \"header\":{\"numRequiredSignatures\":1,"
    "     \"numReadonlySignedAccounts\":0,"
    "     \"numReadonlyUnsignedAccounts\":1},"
    "    \"accountKeys\":[\"" PAYER "\",\"" OTHER "\",\"" SYSTEM_KEY "\"],"
    "    \"recentBlockhash\":\"" RECENT "\","
    "    \"instructions\":[{\"programIdIndex\":2,\"accounts\":[0,1],"
    "     \"data\":\"" DATA_123 "\",\"stackHeight\":1}]}},"
    "  \"meta\":{\"err\":null,\"fee\":5000,"
    "   \"preBalances\":[1000000000,0,1],"
    "   \"postBalances\":[999995000,4000,1],"
    "   \"logMessages\":[\"Program " SYSTEM_KEY " invoke [1]\","
    "    \"Program " SYSTEM_KEY " success\"]}}]}";

static void test_decode_legacy(void) {
    idx_json_doc *doc = parse(LEGACY_BLOCK);
    idx_arena arena;
    idx_arena_init(&arena, 0);
    idx_block block;
    idx_error err;
    idx_error_clear(&err);

    TEST_EQ_INT(idx_block_decode(idx_json_root(doc), 434767210u, &arena, &block,
                                 &err),
                IDX_OK);

    TEST_EQ_UINT(block.slot, 434767210u);
    TEST_EQ_UINT(block.parent_slot, 434767209u);
    TEST_ASSERT(block.has_block_height);
    TEST_EQ_UINT(block.block_height, 412827004u);
    TEST_ASSERT(block.has_block_time);
    TEST_EQ_INT(block.block_time, 1784831561);
    TEST_EQ_UINT(block.transaction_count, 1u);

    const idx_transaction *tx = &block.transactions[0];
    TEST_EQ_INT(tx->version, IDX_TX_VERSION_LEGACY);
    TEST_EQ_UINT(tx->signature_count, 1u);
    TEST_EQ_UINT(tx->account_count, 3u);
    TEST_EQ_UINT(tx->static_account_count, 3u);
    TEST_ASSERT(tx->success);
    TEST_EQ_UINT(tx->fee, 5000u);

    /* Balances: one lamport value per resolved account, before and after. */
    TEST_EQ_UINT(tx->balance_count, 3u);
    TEST_EQ_UINT(tx->pre_balances[0], 1000000000u);
    TEST_EQ_UINT(tx->post_balances[0], 999995000u);
    TEST_EQ_UINT(tx->post_balances[1], 4000u);
    /* No token accounts here, but the logs came through. */
    TEST_EQ_UINT(tx->pre_token_balance_count, 0u);
    TEST_EQ_UINT(tx->post_token_balance_count, 0u);
    TEST_EQ_UINT(tx->log_count, 2u);
    TEST_ASSERT(idx_slice_equal(
        tx->logs[1], idx_slice_from_str("Program " SYSTEM_KEY " success")));

    /* Signer/writable derived from the header: key 0 is the fee payer, key 1 a
     * writable non-signer, key 2 the readonly system program. */
    TEST_ASSERT(tx->accounts[0].is_signer && tx->accounts[0].is_writable);
    TEST_ASSERT(!tx->accounts[1].is_signer && tx->accounts[1].is_writable);
    TEST_ASSERT(!tx->accounts[2].is_signer && !tx->accounts[2].is_writable);
    TEST_ASSERT(!tx->accounts[0].from_lookup_table);
    TEST_ASSERT(idx_pubkey_equal(&tx->accounts[2].pubkey, &IDX_PROGRAM_SYSTEM));

    TEST_EQ_UINT(tx->instruction_count, 1u);
    const idx_instruction *ix = &tx->instructions[0];
    TEST_EQ_UINT(ix->program_id_index, 2u);
    TEST_EQ_UINT(ix->account_count, 2u);
    TEST_EQ_UINT(ix->account_indices[0], 0u);
    TEST_EQ_UINT(ix->account_indices[1], 1u);
    TEST_EQ_UINT(ix->data.len, 3u);
    check_data(ix->data, DATA_123);
    TEST_ASSERT(idx_pubkey_equal(idx_instruction_program_id(tx, ix),
                                 &IDX_PROGRAM_SYSTEM));
    TEST_EQ_UINT(tx->inner_instruction_count, 0u);

    idx_arena_destroy(&arena);
    idx_json_free(doc);
}

static const char *const V0_BLOCK =
    "{\"blockhash\":\"" BLOCKHASH "\",\"parentSlot\":100,"
    " \"transactions\":[{"
    "  \"version\":0,"
    "  \"transaction\":{\"signatures\":[\"" SIG_A "\"],"
    "   \"message\":{"
    "    \"header\":{\"numRequiredSignatures\":1,"
    "     \"numReadonlySignedAccounts\":0,"
    "     \"numReadonlyUnsignedAccounts\":1},"
    "    \"accountKeys\":[\"" PAYER "\",\"" TOKEN_KEY "\"],"
    "    \"recentBlockhash\":\"" RECENT "\","
    "    \"instructions\":[{\"programIdIndex\":1,\"accounts\":[0,2,3],"
    "     \"data\":\"\",\"stackHeight\":1}],"
    "    \"addressTableLookups\":[{\"accountKey\":\"" ALT_KEY "\","
    "     \"writableIndexes\":[5],\"readonlyIndexes\":[6]}]}},"
    "  \"meta\":{\"err\":null,\"fee\":5000,"
    "   \"loadedAddresses\":{\"writable\":[\"" LOADED_W "\"],"
    "    \"readonly\":[\"" LOADED_R "\"]},"
    "   \"preBalances\":[2000000000,0,50,0],"
    "   \"postBalances\":[1999990000,0,50,0],"
    "   \"preTokenBalances\":[{\"accountIndex\":2,\"mint\":\"" OTHER "\","
    "    \"owner\":\"" PAYER "\",\"programId\":\"" TOKEN_KEY "\","
    "    \"uiTokenAmount\":{\"amount\":\"1000000\",\"decimals\":6,"
    "     \"uiAmount\":1.0,\"uiAmountString\":\"1\"}}],"
    "   \"postTokenBalances\":[{\"accountIndex\":2,\"mint\":\"" OTHER "\","
    "    \"uiTokenAmount\":{\"amount\":\"1500000\",\"decimals\":6,"
    "     \"uiAmount\":1.5,\"uiAmountString\":\"1.5\"}}],"
    "   \"logMessages\":[\"Program log: hi\"],"
    "   \"innerInstructions\":[{\"index\":0,\"instructions\":["
    "    {\"programIdIndex\":1,\"accounts\":[2],\"data\":\"" DATA_123 "\","
    "     \"stackHeight\":2}]}]}}]}";

static void test_decode_v0_with_lookups(void) {
    idx_json_doc *doc = parse(V0_BLOCK);
    idx_arena arena;
    idx_arena_init(&arena, 0);
    idx_block block;
    idx_error err;
    idx_error_clear(&err);

    TEST_EQ_INT(
        idx_block_decode(idx_json_root(doc), 500u, &arena, &block, &err),
        IDX_OK);
    /* No blockTime/blockHeight in this fixture: the flags say so. */
    TEST_ASSERT(!block.has_block_height);
    TEST_ASSERT(!block.has_block_time);

    const idx_transaction *tx = &block.transactions[0];
    TEST_EQ_INT(tx->version, IDX_TX_VERSION_0);
    TEST_EQ_UINT(tx->account_count, 4u);
    TEST_EQ_UINT(tx->static_account_count, 2u);

    /* Static key 1 is the readonly, non-signer token program. */
    TEST_ASSERT(idx_pubkey_equal(&tx->accounts[1].pubkey, &IDX_PROGRAM_TOKEN));
    TEST_ASSERT(!tx->accounts[1].is_signer && !tx->accounts[1].is_writable);
    TEST_ASSERT(!tx->accounts[1].from_lookup_table);

    /* Loaded writable comes before loaded readonly, both from a table. */
    TEST_ASSERT(tx->accounts[2].from_lookup_table &&
                tx->accounts[2].is_writable && !tx->accounts[2].is_signer);
    TEST_ASSERT(tx->accounts[3].from_lookup_table &&
                !tx->accounts[3].is_writable && !tx->accounts[3].is_signer);

    const idx_instruction *ix = &tx->instructions[0];
    TEST_EQ_UINT(ix->data.len, 0u);
    TEST_EQ_UINT(ix->account_count, 3u);
    TEST_ASSERT(idx_pubkey_equal(idx_instruction_program_id(tx, ix),
                                 &IDX_PROGRAM_TOKEN));

    TEST_EQ_UINT(tx->inner_instruction_count, 1u);
    const idx_inner_instructions *inner = &tx->inner_instructions[0];
    TEST_EQ_UINT(inner->index, 0u);
    TEST_EQ_UINT(inner->instruction_count, 1u);
    TEST_EQ_UINT(inner->instructions[0].data.len, 3u);
    check_data(inner->instructions[0].data, DATA_123);
    TEST_ASSERT(idx_pubkey_equal(
        idx_instruction_program_id(tx, &inner->instructions[0]),
        &IDX_PROGRAM_TOKEN));

    /* Balances span the full resolved list, static and loaded alike. */
    TEST_EQ_UINT(tx->balance_count, 4u);
    TEST_EQ_UINT(tx->pre_balances[0], 2000000000u);
    TEST_EQ_UINT(tx->post_balances[0], 1999990000u);

    idx_pubkey expected_mint;
    idx_pubkey expected_owner;
    TEST_EQ_INT(idx_pubkey_from_base58(OTHER, strlen(OTHER), &expected_mint,
                                       NULL),
                IDX_OK);
    TEST_EQ_INT(idx_pubkey_from_base58(PAYER, strlen(PAYER), &expected_owner,
                                       NULL),
                IDX_OK);

    /* The pre entry carries owner and programId; the post one omits both, so
     * the optional flags must reflect that. */
    TEST_EQ_UINT(tx->pre_token_balance_count, 1u);
    const idx_token_balance *pre_tb = &tx->pre_token_balances[0];
    TEST_EQ_UINT(pre_tb->account_index, 2u);
    TEST_ASSERT(idx_pubkey_equal(&pre_tb->mint, &expected_mint));
    TEST_ASSERT(pre_tb->has_owner);
    TEST_ASSERT(idx_pubkey_equal(&pre_tb->owner, &expected_owner));
    TEST_ASSERT(pre_tb->has_program_id);
    TEST_ASSERT(idx_pubkey_equal(&pre_tb->program_id, &IDX_PROGRAM_TOKEN));
    TEST_EQ_UINT(pre_tb->amount, 1000000u);
    TEST_EQ_UINT(pre_tb->decimals, 6u);

    TEST_EQ_UINT(tx->post_token_balance_count, 1u);
    const idx_token_balance *post_tb = &tx->post_token_balances[0];
    TEST_EQ_UINT(post_tb->amount, 1500000u);
    TEST_ASSERT(!post_tb->has_owner);
    TEST_ASSERT(!post_tb->has_program_id);

    TEST_EQ_UINT(tx->log_count, 1u);
    TEST_ASSERT(
        idx_slice_equal(tx->logs[0], idx_slice_from_str("Program log: hi")));

    idx_arena_destroy(&arena);
    idx_json_free(doc);
}

/* Decodes `text` and expects a specific failure status. */
static void expect_decode_error(const char *text, idx_status expected) {
    idx_json_doc *doc = parse(text);
    idx_arena arena;
    idx_arena_init(&arena, 0);
    idx_block block;
    idx_error err;
    idx_error_clear(&err);

    idx_status status =
        idx_block_decode(idx_json_root(doc), 1u, &arena, &block, &err);
    TEST_CHECK(status == expected, "expected %s, got %s (%s)",
               idx_status_str(expected), idx_status_str(status), err.message);

    idx_arena_destroy(&arena);
    idx_json_free(doc);
}

/* A minimal one-transaction block with the message and meta spliced in. */
#define BLOCK_WITH(message, meta)                                             \
    "{\"blockhash\":\"" BLOCKHASH "\",\"parentSlot\":1,\"transactions\":[{"    \
    "\"version\":0,\"transaction\":{\"signatures\":[\"" SIG_A "\"],"           \
    "\"message\":" message "},\"meta\":" meta "}]}"

#define BASIC_MESSAGE(instructions)                                           \
    "{\"header\":{\"numRequiredSignatures\":1,"                               \
    "\"numReadonlySignedAccounts\":0,\"numReadonlyUnsignedAccounts\":1},"     \
    "\"accountKeys\":[\"" PAYER "\",\"" SYSTEM_KEY "\"],"                      \
    "\"recentBlockhash\":\"" RECENT "\",\"instructions\":" instructions "}"

static void test_decode_errors(void) {
    /* Not an object. */
    expect_decode_error("[]", IDX_ERR_PARSE);

    /* Unsupported numeric version. */
    expect_decode_error(
        "{\"blockhash\":\"" BLOCKHASH "\",\"parentSlot\":1,\"transactions\":[{"
        "\"version\":2,\"transaction\":{\"signatures\":[],\"message\":"
        BASIC_MESSAGE("[]") "},\"meta\":{}}]}",
        IDX_ERR_PARSE);

    /* Unknown string version. */
    expect_decode_error(
        "{\"blockhash\":\"" BLOCKHASH "\",\"parentSlot\":1,\"transactions\":[{"
        "\"version\":\"banana\",\"transaction\":{\"signatures\":[],\"message\":"
        BASIC_MESSAGE("[]") "},\"meta\":{}}]}",
        IDX_ERR_PARSE);

    /* Instruction account index past the account list (2 accounts, index 9). */
    expect_decode_error(
        BLOCK_WITH(BASIC_MESSAGE("[{\"programIdIndex\":1,\"accounts\":[9],"
                                 "\"data\":\"\"}]"),
                   "{}"),
        IDX_ERR_PARSE);

    /* programIdIndex past the account list. */
    expect_decode_error(
        BLOCK_WITH(BASIC_MESSAGE("[{\"programIdIndex\":7,\"accounts\":[0],"
                                 "\"data\":\"\"}]"),
                   "{}"),
        IDX_ERR_PARSE);

    /* Header requires more signatures than there are static keys. */
    expect_decode_error(
        BLOCK_WITH("{\"header\":{\"numRequiredSignatures\":5,"
                   "\"numReadonlySignedAccounts\":0,"
                   "\"numReadonlyUnsignedAccounts\":0},"
                   "\"accountKeys\":[\"" PAYER "\",\"" SYSTEM_KEY "\"],"
                   "\"recentBlockhash\":\"" RECENT "\",\"instructions\":[]}",
                   "{}"),
        IDX_ERR_PARSE);

    /* A missing required field surfaces from the JSON layer as not-found,
     * distinct from a present field of the wrong type (which is a parse
     * error). Both mean the block is malformed to the caller. */
    expect_decode_error("{\"parentSlot\":1,\"transactions\":[]}",
                        IDX_ERR_NOT_FOUND);

    /* blockhash present but the wrong type is a parse error. */
    expect_decode_error(
        "{\"blockhash\":7,\"parentSlot\":1,\"transactions\":[]}",
        IDX_ERR_PARSE);
}

static void test_decode_metadata_errors(void) {
    /* preBalances and postBalances must agree with the account count (2). */
    expect_decode_error(
        BLOCK_WITH(BASIC_MESSAGE("[]"),
                   "{\"preBalances\":[1],\"postBalances\":[1]}"),
        IDX_ERR_PARSE);

    /* One without the other is malformed. */
    expect_decode_error(
        BLOCK_WITH(BASIC_MESSAGE("[]"), "{\"preBalances\":[1,2]}"),
        IDX_ERR_PARSE);

    /* A token amount past the u64 range is a parse error, not a silent wrap. */
    expect_decode_error(
        BLOCK_WITH(BASIC_MESSAGE("[]"),
                   "{\"preTokenBalances\":[{\"accountIndex\":0,\"mint\":\""
                   OTHER "\",\"uiTokenAmount\":{"
                   "\"amount\":\"99999999999999999999999999\",\"decimals\":6}}]}"),
        IDX_ERR_PARSE);

    /* A non-numeric token amount is rejected. */
    expect_decode_error(
        BLOCK_WITH(BASIC_MESSAGE("[]"),
                   "{\"postTokenBalances\":[{\"accountIndex\":0,\"mint\":\""
                   OTHER "\",\"uiTokenAmount\":{\"amount\":\"12x4\","
                   "\"decimals\":6}}]}"),
        IDX_ERR_PARSE);

    /* A token balance pointing past the account list is out of range. */
    expect_decode_error(
        BLOCK_WITH(BASIC_MESSAGE("[]"),
                   "{\"preTokenBalances\":[{\"accountIndex\":9,\"mint\":\""
                   OTHER "\",\"uiTokenAmount\":{\"amount\":\"1\","
                   "\"decimals\":0}}]}"),
        IDX_ERR_PARSE);
}

/* Meta present but thin: no balances, and logs the runtime truncated to null.
 * None of that is an error; the counts are simply zero. */
static void test_decode_metadata_sparse(void) {
    idx_json_doc *doc = parse(BLOCK_WITH(
        BASIC_MESSAGE("[]"),
        "{\"err\":null,\"fee\":42,\"logMessages\":null}"));
    idx_arena arena;
    idx_arena_init(&arena, 0);
    idx_block block;
    idx_error err;
    idx_error_clear(&err);

    TEST_EQ_INT(
        idx_block_decode(idx_json_root(doc), 1u, &arena, &block, &err),
        IDX_OK);

    const idx_transaction *tx = &block.transactions[0];
    TEST_ASSERT(tx->has_meta);
    TEST_ASSERT(tx->success);
    TEST_EQ_UINT(tx->fee, 42u);
    TEST_EQ_UINT(tx->balance_count, 0u);
    TEST_ASSERT(tx->pre_balances == NULL && tx->post_balances == NULL);
    TEST_EQ_UINT(tx->pre_token_balance_count, 0u);
    TEST_EQ_UINT(tx->post_token_balance_count, 0u);
    TEST_EQ_UINT(tx->log_count, 0u);

    idx_arena_destroy(&arena);
    idx_json_free(doc);

    /* No meta at all — a block fetched with less than full detail. `success`
     * has nothing behind it there, which is what has_meta says: a consumer
     * that must know whether a transaction executed reads both. */
    doc = parse(BLOCK_WITH(BASIC_MESSAGE("[]"), "null"));
    idx_arena_init(&arena, 0);
    TEST_EQ_INT(
        idx_block_decode(idx_json_root(doc), 1u, &arena, &block, &err),
        IDX_OK);
    TEST_ASSERT(!block.transactions[0].has_meta);

    idx_arena_destroy(&arena);
    idx_json_free(doc);
}

/* An empty block — a real slot that produced no transactions — is valid. */
static void test_decode_empty_block(void) {
    idx_json_doc *doc = parse("{\"blockhash\":\"" BLOCKHASH "\","
                              "\"parentSlot\":41,\"transactions\":[]}");
    idx_arena arena;
    idx_arena_init(&arena, 0);
    idx_block block;
    idx_error err;
    idx_error_clear(&err);

    TEST_EQ_INT(
        idx_block_decode(idx_json_root(doc), 42u, &arena, &block, &err),
        IDX_OK);
    TEST_EQ_UINT(block.transaction_count, 0u);
    TEST_EQ_UINT(block.parent_slot, 41u);

    idx_arena_destroy(&arena);
    idx_json_free(doc);
}

TEST_MAIN({
    TEST_RUN(test_decode_legacy);
    TEST_RUN(test_decode_v0_with_lookups);
    TEST_RUN(test_decode_errors);
    TEST_RUN(test_decode_metadata_errors);
    TEST_RUN(test_decode_metadata_sparse);
    TEST_RUN(test_decode_empty_block);
})
