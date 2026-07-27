/*
 * Finalized-tier store backed by ClickHouse. See include/ch_store.h for the
 * shape of the tier and why each table's version column is what it is.
 *
 * The file is three parts: the DDL, one encoder per entity that appends a row
 * to that table's buffer through ch_rows.h, and the vtable that fans a write set
 * across the encoders and flushes each buffer as one insert.
 */
#include "ch_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bar.h"
#include "balance.h"
#include "pool.h"
#include "swap.h"
#include "token.h"
#include "transfer.h"
#include "types.h"

/*
 * The tables, in the order a write set is applied. The enum indexes both the
 * DDL and the per-table row buffers, so adding a table is one entry here, one
 * statement below and one encoder.
 */
typedef enum {
    TBL_BLOCKS = 0,
    TBL_SOL_BALANCES,
    TBL_TOKEN_BALANCES,
    TBL_SOL_TRANSFERS,
    TBL_TOKEN_TRANSFERS,
    TBL_SWAPS,
    TBL_POOLS,
    TBL_TOKENS,
    TBL_BARS_1S,
    TBL_BARS_1M,
    TBL_BARS_1D,
    TBL_COUNT
} table_id;

static const char *const g_table_names[TBL_COUNT] = {
    "blocks",   "sol_balances", "token_balances", "sol_transfers",
    "token_transfers", "swaps", "pools",          "tokens",
    "bars_1s",  "bars_1m",      "bars_1d"};

/* Bars are one table per resolution, so the interval selects the table. */
static table_id bar_table(idx_bar_interval interval) {
    return interval == IDX_BAR_1M ? TBL_BARS_1M : TBL_BARS_1S;
}

/* ------------------------------------------------------------------ DDL -- */

/*
 * Denormalized on purpose (STORAGE.md §5.4): wide tables, no joins.
 *
 * Every table is a ReplacingMergeTree so a re-inserted row converges instead of
 * duplicating — ClickHouse has no upsert (§5.5). Event tables sort by the
 * instruction path, which D5 makes unique; state and dimension tables sort by
 * their natural key so the latest version of an account, a pool or a token wins.
 *
 * Partitioning follows what each table is dropped or rolled up by. Slot-ranged
 * tables take ~10M slots per partition, which is about six weeks at the 2.5
 * slots/s D1a measures — coarse enough to keep the part count low, fine enough
 * that dropping history is a partition drop. Bars partition by month of their
 * bucket, which is what the rollup and the retention of bars_1s work in. State
 * and dimension tables are small and unpartitioned.
 *
 * Codecs where they pay off: Delta then ZSTD on the monotonic slot and bucket
 * columns, which compress to almost nothing once differenced, and ZSTD on the
 * wide key columns, which are the bulk of every event row.
 */
static const char *const g_schema[] = {
    "CREATE TABLE IF NOT EXISTS blocks ("
    "slot UInt64 CODEC(Delta, ZSTD(1)),"
    "blockhash FixedString(32) CODEC(ZSTD(1)),"
    "previous_blockhash FixedString(32) CODEC(ZSTD(1)),"
    "parent_slot UInt64 CODEC(Delta, ZSTD(1)),"
    "block_time Nullable(Int64),"
    "block_height Nullable(UInt64),"
    "transaction_count UInt32"
    ") ENGINE = ReplacingMergeTree(slot)"
    " PARTITION BY intDiv(slot, 10000000)"
    " ORDER BY slot",

    /* State, not a log (D5): one row per account, the slot as the version, so
     * the latest observation wins with no delete. */
    "CREATE TABLE IF NOT EXISTS sol_balances ("
    "account FixedString(32) CODEC(ZSTD(1)),"
    "slot UInt64 CODEC(Delta, ZSTD(1)),"
    "lamports UInt64,"
    "delta Int64,"
    "transaction_index UInt16,"
    "signature FixedString(64) CODEC(ZSTD(1))"
    ") ENGINE = ReplacingMergeTree(slot)"
    " ORDER BY account",

    "CREATE TABLE IF NOT EXISTS token_balances ("
    "account FixedString(32) CODEC(ZSTD(1)),"
    "slot UInt64 CODEC(Delta, ZSTD(1)),"
    "mint FixedString(32) CODEC(ZSTD(1)),"
    "owner Nullable(FixedString(32)),"
    "amount UInt64,"
    "previous UInt64,"
    "decimals UInt8,"
    "closed UInt8,"
    "transaction_index UInt16,"
    "signature FixedString(64) CODEC(ZSTD(1))"
    ") ENGINE = ReplacingMergeTree(slot)"
    " ORDER BY account",

    "CREATE TABLE IF NOT EXISTS sol_transfers ("
    "slot UInt64 CODEC(Delta, ZSTD(1)),"
    "transaction_index UInt16,"
    "instruction_index UInt16,"
    "inner_index UInt16,"
    "signature FixedString(64) CODEC(ZSTD(1)),"
    "source FixedString(32) CODEC(ZSTD(1)),"
    "destination FixedString(32) CODEC(ZSTD(1)),"
    "authority Nullable(FixedString(32)),"
    "amount UInt64"
    ") ENGINE = ReplacingMergeTree(slot)"
    " PARTITION BY intDiv(slot, 10000000)"
    " ORDER BY (slot, transaction_index, instruction_index, inner_index)",

    "CREATE TABLE IF NOT EXISTS token_transfers ("
    "slot UInt64 CODEC(Delta, ZSTD(1)),"
    "transaction_index UInt16,"
    "instruction_index UInt16,"
    "inner_index UInt16,"
    "signature FixedString(64) CODEC(ZSTD(1)),"
    "kind UInt8,"
    "source FixedString(32) CODEC(ZSTD(1)),"
    "destination FixedString(32) CODEC(ZSTD(1)),"
    "authority Nullable(FixedString(32)),"
    "mint Nullable(FixedString(32)),"
    "source_owner Nullable(FixedString(32)),"
    "destination_owner Nullable(FixedString(32)),"
    "amount UInt64,"
    "fee UInt64,"
    "decimals Nullable(UInt8)"
    ") ENGINE = ReplacingMergeTree(slot)"
    " PARTITION BY intDiv(slot, 10000000)"
    " ORDER BY (slot, transaction_index, instruction_index, inner_index)",

    /* Price is a column, not a table (D5): `quote` says which quote the row was
     * denominated in and is set even when no number could be computed. */
    "CREATE TABLE IF NOT EXISTS swaps ("
    "slot UInt64 CODEC(Delta, ZSTD(1)),"
    "transaction_index UInt16,"
    "instruction_index UInt16,"
    "inner_index UInt16,"
    "signature FixedString(64) CODEC(ZSTD(1)),"
    "kind UInt8,"
    "venue UInt8,"
    "amount_source UInt8,"
    "pool Nullable(FixedString(32)),"
    "trader Nullable(FixedString(32)),"
    "input_mint Nullable(FixedString(32)),"
    "input_amount Nullable(UInt64),"
    "input_decimals Nullable(UInt8),"
    "output_mint Nullable(FixedString(32)),"
    "output_amount Nullable(UInt64),"
    "output_decimals Nullable(UInt8),"
    "price Nullable(Float64),"
    "quote Nullable(UInt8),"
    "block_time Nullable(Int64)"
    ") ENGINE = ReplacingMergeTree(slot)"
    " PARTITION BY intDiv(slot, 10000000)"
    " ORDER BY (slot, transaction_index, instruction_index, inner_index)",

    /* Dimensions arrive already merged from the registries, so the most
     * recently written record is the complete one — hence an explicit version
     * column rather than a natural one (see ch_store.h). */
    "CREATE TABLE IF NOT EXISTS pools ("
    "address FixedString(32) CODEC(ZSTD(1)),"
    "venue UInt8,"
    "mint_a Nullable(FixedString(32)),"
    "decimals_a Nullable(UInt8),"
    "mint_b Nullable(FixedString(32)),"
    "decimals_b Nullable(UInt8),"
    "first_seen_slot UInt64,"
    "swap_count UInt64,"
    "creation_slot Nullable(UInt64),"
    "creator Nullable(FixedString(32)),"
    "version UInt64"
    ") ENGINE = ReplacingMergeTree(version)"
    " ORDER BY address",

    "CREATE TABLE IF NOT EXISTS tokens ("
    "mint FixedString(32) CODEC(ZSTD(1)),"
    "decimals Nullable(UInt8),"
    "name Nullable(String),"
    "symbol Nullable(String),"
    "uri Nullable(String),"
    "first_seen_slot UInt64,"
    "version UInt64"
    ") ENGINE = ReplacingMergeTree(version)"
    " ORDER BY mint",

    /*
     * One table per resolution, because the retention and rollup policies
     * differ per resolution: bars_1s is the largest table in the design and is
     * what the rollup to bars_1d bounds.
     *
     * swap_count is the version because a bar is a fold — a later write of the
     * same bucket has folded at least as many swaps as an earlier one, so the
     * count is the fold's own progress and the most-folded row is the one to
     * keep. The packed sequence keys ride along so ordering can still be
     * reasoned about after the fact (bar.h).
     */
#define BARS_TABLE(name)                                                     \
    "CREATE TABLE IF NOT EXISTS " name " ("                                  \
    "pool FixedString(32) CODEC(ZSTD(1)),"                                   \
    "bucket Int64 CODEC(Delta, ZSTD(1)),"                                    \
    "quote UInt8,"                                                           \
    "open Float64,"                                                          \
    "high Float64,"                                                          \
    "low Float64,"                                                           \
    "close Float64,"                                                         \
    "base_volume Float64,"                                                   \
    "quote_volume Float64,"                                                  \
    "swap_count UInt32,"                                                     \
    "open_seq_key FixedString(15),"                                          \
    "close_seq_key FixedString(15),"                                         \
    "close_seq_slot UInt64 CODEC(Delta, ZSTD(1))"                            \
    ") ENGINE = ReplacingMergeTree(swap_count)"                              \
    " PARTITION BY toYYYYMM(toDateTime(greatest(bucket, 0)))"                \
    " ORDER BY (pool, bucket)"

    BARS_TABLE("bars_1s"),
    BARS_TABLE("bars_1m"),
    BARS_TABLE("bars_1d"),
#undef BARS_TABLE

    NULL};

const char *const *idx_ch_finalized_schema(void) { return g_schema; }

/* --------------------------------------------------------------- columns -- */

static idx_status col_key(idx_ch_rows *w, const char *name,
                          const idx_pubkey *key, idx_error *err) {
    return idx_ch_rows_fixed(w, name, key->bytes, IDX_PUBKEY_LEN, err);
}

static idx_status col_key_opt(idx_ch_rows *w, const char *name,
                              const idx_pubkey *key, bool present,
                              idx_error *err) {
    return idx_ch_rows_nullable_fixed(w, name, key->bytes, IDX_PUBKEY_LEN,
                                      present, err);
}

static idx_status col_hash(idx_ch_rows *w, const char *name,
                           const idx_hash *hash, idx_error *err) {
    return idx_ch_rows_fixed(w, name, hash->bytes, IDX_HASH_LEN, err);
}

static idx_status col_sig(idx_ch_rows *w, const char *name,
                          const idx_signature *sig, idx_error *err) {
    return idx_ch_rows_fixed(w, name, sig->bytes, IDX_SIGNATURE_LEN, err);
}

/* A NUL-terminated registry string, written as a Nullable(String). */
static idx_status col_text_opt(idx_ch_rows *w, const char *name,
                               const char *text, bool present, idx_error *err) {
    return idx_ch_rows_nullable_str(w, name, text, present ? strlen(text) : 0,
                                    present, err);
}

static idx_status col_seq_key(idx_ch_rows *w, const char *name,
                              const idx_bar_seq *seq, idx_error *err) {
    uint8_t packed[IDX_BAR_SEQ_KEY_LEN];
    idx_bar_seq_pack(seq, packed);
    return idx_ch_rows_fixed(w, name, packed, sizeof packed, err);
}

/* The five columns every event row starts with: the instruction path D5 keys
 * on, plus the signature that links the row back to an explorer. */
static idx_status col_event_path(idx_ch_rows *w, const idx_store_ref *ref,
                                 uint16_t instruction_index,
                                 uint16_t inner_index, bool inner,
                                 idx_error *err) {
    IDX_TRY(idx_ch_rows_u64(w, "slot", ref->slot, err));
    IDX_TRY(idx_ch_rows_u16(w, "transaction_index", ref->transaction_index,
                            err));
    IDX_TRY(idx_ch_rows_u16(w, "instruction_index", instruction_index, err));
    IDX_TRY(idx_ch_rows_u16(w, "inner_index", inner ? inner_index : 0, err));
    return col_sig(w, "signature", &ref->signature, err);
}

/* -------------------------------------------------------------- encoders -- */

static idx_status put_block(idx_ch_rows *w, const idx_store_block_row *b,
                            idx_error *err) {
    IDX_TRY(idx_ch_rows_begin(w, err));
    IDX_TRY(idx_ch_rows_u64(w, "slot", b->slot, err));
    IDX_TRY(col_hash(w, "blockhash", &b->blockhash, err));
    IDX_TRY(col_hash(w, "previous_blockhash", &b->previous_blockhash, err));
    IDX_TRY(idx_ch_rows_u64(w, "parent_slot", b->parent_slot, err));
    IDX_TRY(idx_ch_rows_nullable_i64(w, "block_time", b->block_time,
                                     b->has_block_time, err));
    IDX_TRY(idx_ch_rows_nullable_u64(w, "block_height", b->block_height,
                                     b->has_block_height, err));
    IDX_TRY(idx_ch_rows_u32(w, "transaction_count", b->transaction_count, err));
    return idx_ch_rows_end(w, err);
}

static idx_status put_sol_balance(idx_ch_rows *w,
                                  const idx_store_sol_balance_row *r,
                                  idx_error *err) {
    const idx_sol_balance *b = &r->balance;
    IDX_TRY(idx_ch_rows_begin(w, err));
    IDX_TRY(col_key(w, "account", &b->account, err));
    IDX_TRY(idx_ch_rows_u64(w, "slot", r->ref.slot, err));
    IDX_TRY(idx_ch_rows_u64(w, "lamports", b->lamports, err));
    IDX_TRY(idx_ch_rows_i64(w, "delta", b->delta, err));
    IDX_TRY(idx_ch_rows_u16(w, "transaction_index", r->ref.transaction_index,
                            err));
    IDX_TRY(col_sig(w, "signature", &r->ref.signature, err));
    return idx_ch_rows_end(w, err);
}

static idx_status put_token_balance(idx_ch_rows *w,
                                    const idx_store_token_balance_row *r,
                                    idx_error *err) {
    const idx_token_balance_state *b = &r->balance;
    IDX_TRY(idx_ch_rows_begin(w, err));
    IDX_TRY(col_key(w, "account", &b->account, err));
    IDX_TRY(idx_ch_rows_u64(w, "slot", r->ref.slot, err));
    IDX_TRY(col_key(w, "mint", &b->mint, err));
    IDX_TRY(col_key_opt(w, "owner", &b->owner, b->has_owner, err));
    IDX_TRY(idx_ch_rows_u64(w, "amount", b->amount, err));
    IDX_TRY(idx_ch_rows_u64(w, "previous", b->previous, err));
    IDX_TRY(idx_ch_rows_u8(w, "decimals", b->decimals, err));
    IDX_TRY(idx_ch_rows_bool(w, "closed", b->closed, err));
    IDX_TRY(idx_ch_rows_u16(w, "transaction_index", r->ref.transaction_index,
                            err));
    IDX_TRY(col_sig(w, "signature", &r->ref.signature, err));
    return idx_ch_rows_end(w, err);
}

static idx_status put_sol_transfer(idx_ch_rows *w,
                                   const idx_store_transfer_row *r,
                                   idx_error *err) {
    const idx_transfer *t = &r->transfer;
    IDX_TRY(idx_ch_rows_begin(w, err));
    IDX_TRY(col_event_path(w, &r->ref, t->instruction_index, t->inner_index,
                           t->inner, err));
    IDX_TRY(col_key(w, "source", &t->source, err));
    IDX_TRY(col_key(w, "destination", &t->destination, err));
    IDX_TRY(col_key_opt(w, "authority", &t->authority, t->has_authority, err));
    IDX_TRY(idx_ch_rows_u64(w, "amount", t->amount, err));
    return idx_ch_rows_end(w, err);
}

static idx_status put_token_transfer(idx_ch_rows *w,
                                     const idx_store_transfer_row *r,
                                     idx_error *err) {
    const idx_transfer *t = &r->transfer;
    IDX_TRY(idx_ch_rows_begin(w, err));
    IDX_TRY(col_event_path(w, &r->ref, t->instruction_index, t->inner_index,
                           t->inner, err));
    IDX_TRY(idx_ch_rows_u8(w, "kind", (uint8_t)t->kind, err));
    IDX_TRY(col_key(w, "source", &t->source, err));
    IDX_TRY(col_key(w, "destination", &t->destination, err));
    IDX_TRY(col_key_opt(w, "authority", &t->authority, t->has_authority, err));
    IDX_TRY(col_key_opt(w, "mint", &t->mint, t->has_mint, err));
    IDX_TRY(col_key_opt(w, "source_owner", &t->source_owner,
                        t->has_source_owner, err));
    IDX_TRY(col_key_opt(w, "destination_owner", &t->destination_owner,
                        t->has_destination_owner, err));
    IDX_TRY(idx_ch_rows_u64(w, "amount", t->amount, err));
    IDX_TRY(idx_ch_rows_u64(w, "fee", t->fee, err));
    IDX_TRY(idx_ch_rows_nullable_u8(w, "decimals", t->decimals,
                                    t->has_decimals, err));
    return idx_ch_rows_end(w, err);
}

static idx_status put_swap(idx_ch_rows *w, const idx_store_swap_row *r,
                           idx_error *err) {
    const idx_swap_row *s = &r->swap;
    IDX_TRY(idx_ch_rows_begin(w, err));
    IDX_TRY(col_event_path(w, &r->ref, s->instruction_index, s->inner_index,
                           s->inner, err));
    IDX_TRY(idx_ch_rows_u8(w, "kind", (uint8_t)s->kind, err));
    IDX_TRY(idx_ch_rows_u8(w, "venue", (uint8_t)s->venue, err));
    IDX_TRY(idx_ch_rows_u8(w, "amount_source", (uint8_t)s->source, err));
    IDX_TRY(col_key_opt(w, "pool", &s->pool, s->has_pool, err));
    IDX_TRY(col_key_opt(w, "trader", &s->user, s->has_user, err));
    IDX_TRY(col_key_opt(w, "input_mint", &s->input_mint, s->has_input_mint,
                        err));
    IDX_TRY(idx_ch_rows_nullable_u64(w, "input_amount", s->input_amount,
                                     s->has_input_amount, err));
    IDX_TRY(idx_ch_rows_nullable_u8(w, "input_decimals", s->input_decimals,
                                    s->has_input_decimals, err));
    IDX_TRY(col_key_opt(w, "output_mint", &s->output_mint, s->has_output_mint,
                        err));
    IDX_TRY(idx_ch_rows_nullable_u64(w, "output_amount", s->output_amount,
                                     s->has_output_amount, err));
    IDX_TRY(idx_ch_rows_nullable_u8(w, "output_decimals", s->output_decimals,
                                    s->has_output_decimals, err));
    IDX_TRY(idx_ch_rows_nullable_f64(w, "price", r->price.price,
                                     r->price.has_price, err));
    IDX_TRY(idx_ch_rows_nullable_u8(w, "quote", (uint8_t)r->price.quote,
                                    r->price.priced, err));
    IDX_TRY(idx_ch_rows_nullable_i64(w, "block_time", r->block_time,
                                     r->has_block_time, err));
    return idx_ch_rows_end(w, err);
}

static idx_status put_pool(idx_ch_rows *w, const idx_pool *p, idx_slot version,
                           idx_error *err) {
    IDX_TRY(idx_ch_rows_begin(w, err));
    IDX_TRY(col_key(w, "address", &p->address, err));
    IDX_TRY(idx_ch_rows_u8(w, "venue", (uint8_t)p->venue, err));
    IDX_TRY(col_key_opt(w, "mint_a", &p->mint_a, p->has_mint_a, err));
    IDX_TRY(idx_ch_rows_nullable_u8(w, "decimals_a", p->decimals_a,
                                    p->has_decimals_a, err));
    IDX_TRY(col_key_opt(w, "mint_b", &p->mint_b, p->has_mint_b, err));
    IDX_TRY(idx_ch_rows_nullable_u8(w, "decimals_b", p->decimals_b,
                                    p->has_decimals_b, err));
    IDX_TRY(idx_ch_rows_u64(w, "first_seen_slot", p->first_seen_slot, err));
    IDX_TRY(idx_ch_rows_u64(w, "swap_count", p->swap_count, err));
    IDX_TRY(idx_ch_rows_nullable_u64(w, "creation_slot", p->creation_slot,
                                     p->has_creation, err));
    IDX_TRY(col_key_opt(w, "creator", &p->creator, p->has_creator, err));
    IDX_TRY(idx_ch_rows_u64(w, "version", version, err));
    return idx_ch_rows_end(w, err);
}

static idx_status put_token(idx_ch_rows *w, const idx_token *t,
                            idx_slot version, idx_error *err) {
    IDX_TRY(idx_ch_rows_begin(w, err));
    IDX_TRY(col_key(w, "mint", &t->mint, err));
    IDX_TRY(idx_ch_rows_nullable_u8(w, "decimals", t->decimals,
                                    t->has_decimals, err));
    IDX_TRY(col_text_opt(w, "name", t->name, t->has_name, err));
    IDX_TRY(col_text_opt(w, "symbol", t->symbol, t->has_symbol, err));
    IDX_TRY(col_text_opt(w, "uri", t->uri, t->has_uri, err));
    IDX_TRY(idx_ch_rows_u64(w, "first_seen_slot", t->first_seen_slot, err));
    IDX_TRY(idx_ch_rows_u64(w, "version", version, err));
    return idx_ch_rows_end(w, err);
}

static idx_status put_bar(idx_ch_rows *w, const idx_bar *b, idx_error *err) {
    IDX_TRY(idx_ch_rows_begin(w, err));
    IDX_TRY(col_key(w, "pool", &b->pool, err));
    IDX_TRY(idx_ch_rows_i64(w, "bucket", b->bucket, err));
    IDX_TRY(idx_ch_rows_u8(w, "quote", (uint8_t)b->quote, err));
    IDX_TRY(idx_ch_rows_f64(w, "open", b->open, err));
    IDX_TRY(idx_ch_rows_f64(w, "high", b->high, err));
    IDX_TRY(idx_ch_rows_f64(w, "low", b->low, err));
    IDX_TRY(idx_ch_rows_f64(w, "close", b->close, err));
    IDX_TRY(idx_ch_rows_f64(w, "base_volume", b->base_volume, err));
    IDX_TRY(idx_ch_rows_f64(w, "quote_volume", b->quote_volume, err));
    IDX_TRY(idx_ch_rows_u32(w, "swap_count", b->swap_count, err));
    IDX_TRY(col_seq_key(w, "open_seq_key", &b->open_seq, err));
    IDX_TRY(col_seq_key(w, "close_seq_key", &b->close_seq, err));
    IDX_TRY(idx_ch_rows_u64(w, "close_seq_slot", b->close_seq.slot, err));
    return idx_ch_rows_end(w, err);
}

/* ---------------------------------------------------------------- store -- */

typedef struct {
    idx_ch_conn *conn;
    idx_ch_row_format format;
    idx_ch_rows rows[TBL_COUNT];
} ch_store;

static const char *store_name(void *ctx) {
    (void)ctx;
    return "clickhouse-finalized";
}

/*
 * The version for the dimension rows of this write set: the highest slot it
 * carries. A dimension has no monotonic column of its own, and the record
 * arrives already merged, so "written from a later slot" is exactly the
 * ordering ReplacingMergeTree needs. Taking it from the set rather than from a
 * counter keeps it durable across restarts.
 */
static void bump(idx_slot *best, idx_slot candidate) {
    if (candidate != IDX_SLOT_NONE && candidate > *best) {
        *best = candidate;
    }
}

static idx_slot write_set_version(const idx_store_write_set *set) {
    idx_slot version = 0;
    for (size_t i = 0; i < set->block_count; i++) {
        bump(&version, set->blocks[i].slot);
    }
    for (size_t i = 0; i < set->sol_balance_count; i++) {
        bump(&version, set->sol_balances[i].ref.slot);
    }
    for (size_t i = 0; i < set->token_balance_count; i++) {
        bump(&version, set->token_balances[i].ref.slot);
    }
    for (size_t i = 0; i < set->transfer_count; i++) {
        bump(&version, set->transfers[i].ref.slot);
    }
    for (size_t i = 0; i < set->swap_count; i++) {
        bump(&version, set->swaps[i].ref.slot);
    }
    for (size_t i = 0; i < set->pool_count; i++) {
        bump(&version, set->pools[i].first_seen_slot);
        if (set->pools[i].has_creation) {
            bump(&version, set->pools[i].creation_slot);
        }
    }
    for (size_t i = 0; i < set->token_count; i++) {
        bump(&version, set->tokens[i].first_seen_slot);
    }
    for (size_t i = 0; i < set->bar_count; i++) {
        bump(&version, set->bars[i].close_seq.slot);
    }
    return version;
}

static idx_status store_append(void *ctx, const idx_store_write_set *set,
                               idx_error *err) {
    ch_store *s = ctx;
    if (set == NULL) {
        return IDX_OK;
    }
    const idx_slot version = write_set_version(set);

    for (size_t i = 0; i < set->block_count; i++) {
        IDX_TRY(put_block(&s->rows[TBL_BLOCKS], &set->blocks[i], err));
    }
    for (size_t i = 0; i < set->sol_balance_count; i++) {
        IDX_TRY(put_sol_balance(&s->rows[TBL_SOL_BALANCES],
                                &set->sol_balances[i], err));
    }
    for (size_t i = 0; i < set->token_balance_count; i++) {
        IDX_TRY(put_token_balance(&s->rows[TBL_TOKEN_BALANCES],
                                  &set->token_balances[i], err));
    }
    /* Transfers split by kind (D5): SOL to one table, the rest to the other. */
    for (size_t i = 0; i < set->transfer_count; i++) {
        const idx_store_transfer_row *r = &set->transfers[i];
        IDX_TRY(r->transfer.kind == IDX_TRANSFER_SOL
                    ? put_sol_transfer(&s->rows[TBL_SOL_TRANSFERS], r, err)
                    : put_token_transfer(&s->rows[TBL_TOKEN_TRANSFERS], r,
                                         err));
    }
    for (size_t i = 0; i < set->swap_count; i++) {
        IDX_TRY(put_swap(&s->rows[TBL_SWAPS], &set->swaps[i], err));
    }
    for (size_t i = 0; i < set->pool_count; i++) {
        IDX_TRY(put_pool(&s->rows[TBL_POOLS], &set->pools[i], version, err));
    }
    for (size_t i = 0; i < set->token_count; i++) {
        IDX_TRY(put_token(&s->rows[TBL_TOKENS], &set->tokens[i], version, err));
    }
    for (size_t i = 0; i < set->bar_count; i++) {
        const idx_bar *b = &set->bars[i];
        IDX_TRY(put_bar(&s->rows[bar_table(b->interval)], b, err));
    }
    return IDX_OK;
}

/*
 * One insert per non-empty table, each reset as soon as it lands. There is no
 * transaction across tables, so a failure part-way leaves the earlier tables
 * written and the failing one — plus everything after it — still buffered: the
 * caller retries the same flush, and the rows that landed twice collapse on
 * merge because every table is a ReplacingMergeTree.
 */
static idx_status store_flush(void *ctx, idx_error *err) {
    ch_store *s = ctx;
    char sql[128];
    for (size_t i = 0; i < TBL_COUNT; i++) {
        idx_ch_rows *w = &s->rows[i];
        if (idx_ch_rows_count(w) == 0) {
            continue;
        }
        snprintf(sql, sizeof sql, "INSERT INTO %s FORMAT %s",
                 g_table_names[i], idx_ch_row_format_name(s->format));
        idx_slice body = idx_ch_rows_body(w);
        IDX_TRY(idx_ch_insert(s->conn, sql, body.data, body.len, err));
        idx_ch_rows_reset(w);
    }
    return IDX_OK;
}

static void store_close(void *ctx) {
    ch_store *s = ctx;
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < TBL_COUNT; i++) {
        idx_ch_rows_free(&s->rows[i]);
    }
    idx_ch_close(s->conn);
    free(s);
}

static const idx_finalized_store_vt g_vt = {
    .name = store_name,
    .append = store_append,
    .flush = store_flush,
    .close = store_close,
};

/* The context, but only for a handle this module returned. */
static const ch_store *store_of(const idx_finalized_store *store) {
    if (store == NULL || store->vt != &g_vt) {
        return NULL;
    }
    return store->ctx;
}

size_t idx_ch_finalized_store_pending(const idx_finalized_store *store) {
    const ch_store *s = store_of(store);
    if (s == NULL) {
        return 0;
    }
    size_t total = 0;
    for (size_t i = 0; i < TBL_COUNT; i++) {
        total += idx_ch_rows_count(&s->rows[i]);
    }
    return total;
}

size_t idx_ch_finalized_store_pending_bytes(const idx_finalized_store *store) {
    const ch_store *s = store_of(store);
    if (s == NULL) {
        return 0;
    }
    size_t total = 0;
    for (size_t i = 0; i < TBL_COUNT; i++) {
        total += idx_ch_rows_size(&s->rows[i]);
    }
    return total;
}

idx_status idx_ch_finalized_store_open(const idx_ch_options *options,
                                       idx_ch_row_format format,
                                       idx_finalized_store **out,
                                       idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "ch finalized store: null out");
    }
    idx_ch_conn *conn = NULL;
    IDX_TRY(idx_ch_open(options, &conn, err));

    for (size_t i = 0; g_schema[i] != NULL; i++) {
        idx_status st = idx_ch_query(conn, g_schema[i], NULL, err);
        if (st != IDX_OK) {
            idx_ch_close(conn);
            return st;
        }
    }

    idx_finalized_store *store = calloc(1, sizeof *store);
    ch_store *s = calloc(1, sizeof *s);
    if (store == NULL || s == NULL) {
        free(store);
        free(s);
        idx_ch_close(conn);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "ch finalized store: alloc");
    }
    s->conn = conn;
    s->format = format;
    for (size_t i = 0; i < TBL_COUNT; i++) {
        idx_ch_rows_init(&s->rows[i], format);
    }
    store->vt = &g_vt;
    store->ctx = s;
    *out = store;
    return IDX_OK;
}
