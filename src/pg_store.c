/*
 * Confirmed-tier store over PostgreSQL. See include/pg_store.h for the contract.
 *
 * Compiles to nothing unless IDX_HAVE_LIBPQ is defined (the Makefile defines it
 * when it finds libpq), like src/pg.c.
 *
 * This commit implements the schema and the write path (decision D4/D5): blocks
 * and dimensions upserted on their key, events keyed on the instruction path
 * and idempotent, balance state upserted on the account and versioned by slot,
 * bars merged with the greatest/least/+ folding D4 describes. The reorg,
 * retention and promotion-read operations are their own roadmap items and are
 * left unset in the vtable until then; the store.h dispatch reports them as
 * unsupported rather than pretending.
 */
#include "pg_store.h"

#ifdef IDX_HAVE_LIBPQ

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pg.h"

/* -------------------------------------------------------------- schema ----- */

/*
 * The tables the confirmed tier holds (D5). Every one carries `slot` (or, for
 * bars, close_seq_slot) and is indexed by it, so the reorg delete and the
 * retention prune are range operations rather than scans (D4). uint64 amounts
 * that overflow int64 (raw token amounts, balance.h) are NUMERIC(20,0);
 * lamports fit int64 and stay BIGINT. Keys and signatures are BYTEA.
 */
static const char *const g_schema[] = {
    "CREATE TABLE IF NOT EXISTS blocks ("
    "  slot BIGINT PRIMARY KEY,"
    "  blockhash BYTEA NOT NULL,"
    "  previous_blockhash BYTEA NOT NULL,"
    "  parent_slot BIGINT NOT NULL,"
    "  block_time BIGINT,"
    "  block_height BIGINT,"
    "  transaction_count INTEGER NOT NULL)",

    "CREATE TABLE IF NOT EXISTS sol_balances ("
    "  account BYTEA PRIMARY KEY,"
    "  slot BIGINT NOT NULL,"
    "  lamports BIGINT NOT NULL,"
    "  delta BIGINT NOT NULL,"
    "  transaction_index INTEGER NOT NULL,"
    "  signature BYTEA NOT NULL)",
    "CREATE INDEX IF NOT EXISTS sol_balances_slot ON sol_balances (slot)",

    "CREATE TABLE IF NOT EXISTS token_balances ("
    "  account BYTEA PRIMARY KEY,"
    "  slot BIGINT NOT NULL,"
    "  mint BYTEA NOT NULL,"
    "  owner BYTEA,"
    "  amount NUMERIC(20,0) NOT NULL,"
    "  previous NUMERIC(20,0) NOT NULL,"
    "  decimals SMALLINT NOT NULL,"
    "  closed BOOLEAN NOT NULL,"
    "  transaction_index INTEGER NOT NULL,"
    "  signature BYTEA NOT NULL)",
    "CREATE INDEX IF NOT EXISTS token_balances_slot ON token_balances (slot)",

    "CREATE TABLE IF NOT EXISTS sol_transfers ("
    "  slot BIGINT NOT NULL,"
    "  transaction_index INTEGER NOT NULL,"
    "  instruction_index INTEGER NOT NULL,"
    "  inner_index INTEGER NOT NULL,"
    "  signature BYTEA NOT NULL,"
    "  source BYTEA NOT NULL,"
    "  destination BYTEA NOT NULL,"
    "  authority BYTEA,"
    "  amount NUMERIC(20,0) NOT NULL,"
    "  PRIMARY KEY (slot, transaction_index, instruction_index, inner_index))",

    "CREATE TABLE IF NOT EXISTS token_transfers ("
    "  slot BIGINT NOT NULL,"
    "  transaction_index INTEGER NOT NULL,"
    "  instruction_index INTEGER NOT NULL,"
    "  inner_index INTEGER NOT NULL,"
    "  signature BYTEA NOT NULL,"
    "  kind SMALLINT NOT NULL,"
    "  source BYTEA NOT NULL,"
    "  destination BYTEA NOT NULL,"
    "  authority BYTEA,"
    "  mint BYTEA,"
    "  source_owner BYTEA,"
    "  destination_owner BYTEA,"
    "  amount NUMERIC(20,0) NOT NULL,"
    "  fee NUMERIC(20,0) NOT NULL,"
    "  decimals SMALLINT,"
    "  PRIMARY KEY (slot, transaction_index, instruction_index, inner_index))",

    "CREATE TABLE IF NOT EXISTS swaps ("
    "  slot BIGINT NOT NULL,"
    "  transaction_index INTEGER NOT NULL,"
    "  instruction_index INTEGER NOT NULL,"
    "  inner_index INTEGER NOT NULL,"
    "  signature BYTEA NOT NULL,"
    "  kind SMALLINT NOT NULL,"
    "  venue SMALLINT NOT NULL,"
    "  amount_source SMALLINT NOT NULL,"
    "  pool BYTEA,"
    "  trader BYTEA,"
    "  input_mint BYTEA,"
    "  input_amount NUMERIC(20,0),"
    "  input_decimals SMALLINT,"
    "  output_mint BYTEA,"
    "  output_amount NUMERIC(20,0),"
    "  output_decimals SMALLINT,"
    "  price DOUBLE PRECISION,"
    "  quote SMALLINT,"
    "  block_time BIGINT,"
    "  PRIMARY KEY (slot, transaction_index, instruction_index, inner_index))",
    "CREATE INDEX IF NOT EXISTS swaps_pool ON swaps (pool)",

    "CREATE TABLE IF NOT EXISTS pools ("
    "  address BYTEA PRIMARY KEY,"
    "  venue SMALLINT NOT NULL,"
    "  mint_a BYTEA,"
    "  decimals_a SMALLINT,"
    "  mint_b BYTEA,"
    "  decimals_b SMALLINT,"
    "  first_seen_slot BIGINT NOT NULL,"
    "  swap_count BIGINT NOT NULL,"
    "  creation_slot BIGINT,"
    "  creator BYTEA)",
    "CREATE INDEX IF NOT EXISTS pools_slot ON pools (first_seen_slot)",

    "CREATE TABLE IF NOT EXISTS tokens ("
    "  mint BYTEA PRIMARY KEY,"
    "  decimals SMALLINT,"
    "  name TEXT,"
    "  symbol TEXT,"
    "  uri TEXT,"
    "  first_seen_slot BIGINT NOT NULL)",
    "CREATE INDEX IF NOT EXISTS tokens_slot ON tokens (first_seen_slot)",

    "CREATE TABLE IF NOT EXISTS bars ("
    "  pool BYTEA NOT NULL,"
    "  interval SMALLINT NOT NULL,"
    "  bucket BIGINT NOT NULL,"
    "  quote SMALLINT NOT NULL,"
    "  open DOUBLE PRECISION NOT NULL,"
    "  high DOUBLE PRECISION NOT NULL,"
    "  low DOUBLE PRECISION NOT NULL,"
    "  close DOUBLE PRECISION NOT NULL,"
    "  base_volume DOUBLE PRECISION NOT NULL,"
    "  quote_volume DOUBLE PRECISION NOT NULL,"
    "  swap_count BIGINT NOT NULL,"
    "  open_seq_key BYTEA NOT NULL,"
    "  close_seq_key BYTEA NOT NULL,"
    "  close_seq_slot BIGINT NOT NULL,"
    "  PRIMARY KEY (pool, interval, bucket))",
    "CREATE INDEX IF NOT EXISTS bars_close_slot ON bars (close_seq_slot)",

    NULL,
};

const char *const *idx_pg_confirmed_schema(void) { return g_schema; }

/* --------------------------------------------------- prepared statements --- */

/* One (name, sql, n_params) per write. Prepared on open, executed per row. */
typedef struct {
    const char *name;
    const char *sql;
    int n_params;
} stmt_def;

static const stmt_def g_stmts[] = {
    {"put_block",
     "INSERT INTO blocks(slot, blockhash, previous_blockhash, parent_slot,"
     " block_time, block_height, transaction_count)"
     " VALUES($1::bigint,$2::bytea,$3::bytea,$4::bigint,$5::bigint,$6::bigint,"
     "$7::int)"
     " ON CONFLICT (slot) DO UPDATE SET blockhash=excluded.blockhash,"
     " previous_blockhash=excluded.previous_blockhash,"
     " parent_slot=excluded.parent_slot, block_time=excluded.block_time,"
     " block_height=excluded.block_height,"
     " transaction_count=excluded.transaction_count",
     7},

    /* State upserts keep the newest observation: an older backfilled slot must
     * not clobber a newer value, hence the slot guard in the WHERE. */
    {"put_sol_balance",
     "INSERT INTO sol_balances(account, slot, lamports, delta,"
     " transaction_index, signature)"
     " VALUES($1::bytea,$2::bigint,$3::bigint,$4::bigint,$5::int,$6::bytea)"
     " ON CONFLICT (account) DO UPDATE SET slot=excluded.slot,"
     " lamports=excluded.lamports, delta=excluded.delta,"
     " transaction_index=excluded.transaction_index,"
     " signature=excluded.signature WHERE sol_balances.slot <= excluded.slot",
     6},

    {"put_token_balance",
     "INSERT INTO token_balances(account, slot, mint, owner, amount, previous,"
     " decimals, closed, transaction_index, signature)"
     " VALUES($1::bytea,$2::bigint,$3::bytea,$4::bytea,$5::numeric,$6::numeric,"
     "$7::smallint,$8::boolean,$9::int,$10::bytea)"
     " ON CONFLICT (account) DO UPDATE SET slot=excluded.slot,"
     " mint=excluded.mint, owner=excluded.owner, amount=excluded.amount,"
     " previous=excluded.previous, decimals=excluded.decimals,"
     " closed=excluded.closed, transaction_index=excluded.transaction_index,"
     " signature=excluded.signature WHERE token_balances.slot <= excluded.slot",
     10},

    {"put_sol_transfer",
     "INSERT INTO sol_transfers(slot, transaction_index, instruction_index,"
     " inner_index, signature, source, destination, authority, amount)"
     " VALUES($1::bigint,$2::int,$3::int,$4::int,$5::bytea,$6::bytea,$7::bytea,"
     "$8::bytea,$9::numeric)"
     " ON CONFLICT (slot, transaction_index, instruction_index, inner_index)"
     " DO NOTHING",
     9},

    {"put_token_transfer",
     "INSERT INTO token_transfers(slot, transaction_index, instruction_index,"
     " inner_index, signature, kind, source, destination, authority, mint,"
     " source_owner, destination_owner, amount, fee, decimals)"
     " VALUES($1::bigint,$2::int,$3::int,$4::int,$5::bytea,$6::smallint,"
     "$7::bytea,$8::bytea,$9::bytea,$10::bytea,$11::bytea,$12::bytea,"
     "$13::numeric,$14::numeric,$15::smallint)"
     " ON CONFLICT (slot, transaction_index, instruction_index, inner_index)"
     " DO NOTHING",
     15},

    {"put_swap",
     "INSERT INTO swaps(slot, transaction_index, instruction_index,"
     " inner_index, signature, kind, venue, amount_source, pool, trader,"
     " input_mint, input_amount, input_decimals, output_mint, output_amount,"
     " output_decimals, price, quote, block_time)"
     " VALUES($1::bigint,$2::int,$3::int,$4::int,$5::bytea,$6::smallint,"
     "$7::smallint,$8::smallint,$9::bytea,$10::bytea,$11::bytea,$12::numeric,"
     "$13::smallint,$14::bytea,$15::numeric,$16::smallint,$17::double precision,"
     "$18::smallint,$19::bigint)"
     " ON CONFLICT (slot, transaction_index, instruction_index, inner_index)"
     " DO NOTHING",
     19},

    /* Dimensions accumulate: a later row fills what an earlier one left null,
     * so coalesce keeps the first non-null and least keeps the earliest slot. */
    {"put_pool",
     "INSERT INTO pools(address, venue, mint_a, decimals_a, mint_b, decimals_b,"
     " first_seen_slot, swap_count, creation_slot, creator)"
     " VALUES($1::bytea,$2::smallint,$3::bytea,$4::smallint,$5::bytea,"
     "$6::smallint,$7::bigint,$8::bigint,$9::bigint,$10::bytea)"
     " ON CONFLICT (address) DO UPDATE SET venue=excluded.venue,"
     " mint_a=coalesce(pools.mint_a, excluded.mint_a),"
     " decimals_a=coalesce(pools.decimals_a, excluded.decimals_a),"
     " mint_b=coalesce(pools.mint_b, excluded.mint_b),"
     " decimals_b=coalesce(pools.decimals_b, excluded.decimals_b),"
     " first_seen_slot=least(pools.first_seen_slot, excluded.first_seen_slot),"
     " swap_count=greatest(pools.swap_count, excluded.swap_count),"
     " creation_slot=coalesce(pools.creation_slot, excluded.creation_slot),"
     " creator=coalesce(pools.creator, excluded.creator)",
     10},

    {"put_token",
     "INSERT INTO tokens(mint, decimals, name, symbol, uri, first_seen_slot)"
     " VALUES($1::bytea,$2::smallint,$3::text,$4::text,$5::text,$6::bigint)"
     " ON CONFLICT (mint) DO UPDATE SET"
     " decimals=coalesce(tokens.decimals, excluded.decimals),"
     " name=coalesce(tokens.name, excluded.name),"
     " symbol=coalesce(tokens.symbol, excluded.symbol),"
     " uri=coalesce(tokens.uri, excluded.uri),"
     " first_seen_slot=least(tokens.first_seen_slot, excluded.first_seen_slot)",
     6},

    /* Bars fold (D4): high/low by extremes, volumes and count summed, open and
     * close by execution order via the packed seq keys. */
    {"put_bar",
     "INSERT INTO bars(pool, interval, bucket, quote, open, high, low, close,"
     " base_volume, quote_volume, swap_count, open_seq_key, close_seq_key,"
     " close_seq_slot)"
     " VALUES($1::bytea,$2::smallint,$3::bigint,$4::smallint,"
     "$5::double precision,$6::double precision,$7::double precision,"
     "$8::double precision,$9::double precision,$10::double precision,"
     "$11::bigint,$12::bytea,$13::bytea,$14::bigint)"
     " ON CONFLICT (pool, interval, bucket) DO UPDATE SET"
     " high=greatest(bars.high, excluded.high),"
     " low=least(bars.low, excluded.low),"
     " base_volume=bars.base_volume + excluded.base_volume,"
     " quote_volume=bars.quote_volume + excluded.quote_volume,"
     " swap_count=bars.swap_count + excluded.swap_count,"
     " quote=excluded.quote,"
     " open=CASE WHEN excluded.open_seq_key < bars.open_seq_key"
     " THEN excluded.open ELSE bars.open END,"
     " open_seq_key=least(bars.open_seq_key, excluded.open_seq_key),"
     " close=CASE WHEN excluded.close_seq_key > bars.close_seq_key"
     " THEN excluded.close ELSE bars.close END,"
     " close_seq_key=greatest(bars.close_seq_key, excluded.close_seq_key),"
     " close_seq_slot=greatest(bars.close_seq_slot, excluded.close_seq_slot)",
     14},

    /* Reorg deletes (D4): everything at or above the reorged slot. State and
     * event tables delete by `slot`, the dimensions by their first-seen slot,
     * and bars by the slot of their latest swap (close_seq_slot) — a bar holds
     * a reorged swap exactly when that is at or above the cut. */
    {"del_blocks", "DELETE FROM blocks WHERE slot >= $1::bigint", 1},
    {"del_sol_balances", "DELETE FROM sol_balances WHERE slot >= $1::bigint", 1},
    {"del_token_balances",
     "DELETE FROM token_balances WHERE slot >= $1::bigint", 1},
    {"del_sol_transfers", "DELETE FROM sol_transfers WHERE slot >= $1::bigint",
     1},
    {"del_token_transfers",
     "DELETE FROM token_transfers WHERE slot >= $1::bigint", 1},
    {"del_swaps", "DELETE FROM swaps WHERE slot >= $1::bigint", 1},
    {"del_pools", "DELETE FROM pools WHERE first_seen_slot >= $1::bigint", 1},
    {"del_tokens", "DELETE FROM tokens WHERE first_seen_slot >= $1::bigint", 1},
    {"del_bars", "DELETE FROM bars WHERE close_seq_slot >= $1::bigint", 1},
};

/* The reorg deletes, in the order store_reorg runs them (any order is correct;
 * there are no foreign keys between the tables). */
static const char *const g_reorg_deletes[] = {
    "del_blocks",        "del_sol_balances",   "del_token_balances",
    "del_sol_transfers", "del_token_transfers", "del_swaps",
    "del_pools",         "del_tokens",          "del_bars"};

#define STMT_COUNT (sizeof(g_stmts) / sizeof(g_stmts[0]))

/* ---------------------------------------------------- value formatting ----- */

/* A BYTEA text-format literal ("\xdeadbeef"): 2 sentinel + 2 hex per byte. */
#define BYTEA_BUF(n) (2 + 2 * (n) + 1)
#define PUBKEY_BUF BYTEA_BUF(IDX_PUBKEY_LEN)
#define SIG_BUF BYTEA_BUF(IDX_SIGNATURE_LEN)
#define NUM_BUF 24    /* a uint64/int64 in decimal, with room to spare */
#define DBL_BUF 32
#define SEQ_KEY_BUF BYTEA_BUF(15)

static void bytea_hex(const uint8_t *bytes, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    out[0] = '\\';
    out[1] = 'x';
    for (size_t i = 0; i < n; i++) {
        out[2 + 2 * i] = hex[bytes[i] >> 4];
        out[2 + 2 * i + 1] = hex[bytes[i] & 0x0f];
    }
    out[2 + 2 * n] = '\0';
}

static void pubkey_hex(const idx_pubkey *key, char *out) {
    bytea_hex(key->bytes, IDX_PUBKEY_LEN, out);
}

/* Packs a bar sequence big-endian into a 15-byte key whose bytewise order
 * matches idx_bar_seq_compare: slot, transaction, instruction, the inner flag
 * (top-level before inner), then inner_index. */
static void seq_key_hex(const idx_bar_seq *seq, char *out) {
    uint8_t raw[15];
    for (int i = 0; i < 8; i++) {
        raw[i] = (uint8_t)(seq->slot >> (8 * (7 - i)));
    }
    raw[8] = (uint8_t)(seq->transaction_index >> 8);
    raw[9] = (uint8_t)seq->transaction_index;
    raw[10] = (uint8_t)(seq->instruction_index >> 8);
    raw[11] = (uint8_t)seq->instruction_index;
    raw[12] = seq->inner ? 1 : 0;
    raw[13] = (uint8_t)(seq->inner_index >> 8);
    raw[14] = (uint8_t)seq->inner_index;
    bytea_hex(raw, sizeof raw, out);
}

static void u64_str(uint64_t v, char *out) {
    snprintf(out, NUM_BUF, "%" PRIu64, v);
}
static void i64_str(int64_t v, char *out) {
    snprintf(out, NUM_BUF, "%" PRId64, v);
}
static void dbl_str(double v, char *out) { snprintf(out, DBL_BUF, "%.17g", v); }

/* -------------------------------------------------------------- store ------ */

typedef struct {
    idx_pg_conn *conn;
} pg_store;

static const char *store_name(void *ctx) {
    (void)ctx;
    return "postgres-confirmed";
}

/* Runs one prepared write, mapping a NULL element to SQL NULL. */
static idx_status put(pg_store *s, const char *name, int n,
                      const char *const *values, idx_error *err) {
    return idx_pg_exec_prepared(s->conn, name, n, values, NULL, err);
}

static idx_status put_block(pg_store *s, const idx_store_block_row *b,
                            idx_error *err) {
    char slot[NUM_BUF], bh[PUBKEY_BUF], pbh[PUBKEY_BUF], parent[NUM_BUF];
    char btime[NUM_BUF], bheight[NUM_BUF], txc[NUM_BUF];
    u64_str(b->slot, slot);
    bytea_hex(b->blockhash.bytes, IDX_HASH_LEN, bh);
    bytea_hex(b->previous_blockhash.bytes, IDX_HASH_LEN, pbh);
    u64_str(b->parent_slot, parent);
    i64_str(b->block_time, btime);
    u64_str(b->block_height, bheight);
    u64_str(b->transaction_count, txc);
    const char *v[7] = {slot,
                        bh,
                        pbh,
                        parent,
                        b->has_block_time ? btime : NULL,
                        b->has_block_height ? bheight : NULL,
                        txc};
    return put(s, "put_block", 7, v, err);
}

static idx_status put_sol_balance(pg_store *s,
                                  const idx_store_sol_balance_row *r,
                                  idx_error *err) {
    char acct[PUBKEY_BUF], slot[NUM_BUF], lamports[NUM_BUF], delta[NUM_BUF];
    char txi[NUM_BUF], sig[SIG_BUF];
    pubkey_hex(&r->balance.account, acct);
    u64_str(r->ref.slot, slot);
    u64_str(r->balance.lamports, lamports);
    i64_str(r->balance.delta, delta);
    u64_str(r->ref.transaction_index, txi);
    bytea_hex(r->ref.signature.bytes, IDX_SIGNATURE_LEN, sig);
    const char *v[6] = {acct, slot, lamports, delta, txi, sig};
    return put(s, "put_sol_balance", 6, v, err);
}

static idx_status put_token_balance(pg_store *s,
                                    const idx_store_token_balance_row *r,
                                    idx_error *err) {
    char acct[PUBKEY_BUF], slot[NUM_BUF], mint[PUBKEY_BUF], owner[PUBKEY_BUF];
    char amount[NUM_BUF], previous[NUM_BUF], decimals[NUM_BUF], txi[NUM_BUF];
    char sig[SIG_BUF];
    pubkey_hex(&r->balance.account, acct);
    u64_str(r->ref.slot, slot);
    pubkey_hex(&r->balance.mint, mint);
    pubkey_hex(&r->balance.owner, owner);
    u64_str(r->balance.amount, amount);
    u64_str(r->balance.previous, previous);
    u64_str(r->balance.decimals, decimals);
    u64_str(r->ref.transaction_index, txi);
    bytea_hex(r->ref.signature.bytes, IDX_SIGNATURE_LEN, sig);
    const char *v[10] = {acct,
                         slot,
                         mint,
                         r->balance.has_owner ? owner : NULL,
                         amount,
                         previous,
                         decimals,
                         r->balance.closed ? "true" : "false",
                         txi,
                         sig};
    return put(s, "put_token_balance", 10, v, err);
}

/* Shared prefix of a transfer/swap event: the four instruction-path columns
 * and the signature. */
static void event_path(const idx_store_ref *ref, uint16_t instruction_index,
                       uint16_t inner_index, bool inner, char slot[NUM_BUF],
                       char txi[NUM_BUF], char ixi[NUM_BUF], char inr[NUM_BUF],
                       char sig[SIG_BUF]) {
    u64_str(ref->slot, slot);
    u64_str(ref->transaction_index, txi);
    u64_str(instruction_index, ixi);
    u64_str(inner ? inner_index : 0, inr);
    bytea_hex(ref->signature.bytes, IDX_SIGNATURE_LEN, sig);
}

static idx_status put_sol_transfer(pg_store *s, const idx_store_transfer_row *r,
                                   idx_error *err) {
    const idx_transfer *t = &r->transfer;
    char slot[NUM_BUF], txi[NUM_BUF], ixi[NUM_BUF], inr[NUM_BUF], sig[SIG_BUF];
    char src[PUBKEY_BUF], dst[PUBKEY_BUF], auth[PUBKEY_BUF], amount[NUM_BUF];
    event_path(&r->ref, t->instruction_index, t->inner_index, t->inner, slot,
               txi, ixi, inr, sig);
    pubkey_hex(&t->source, src);
    pubkey_hex(&t->destination, dst);
    pubkey_hex(&t->authority, auth);
    u64_str(t->amount, amount);
    const char *v[9] = {slot, txi, ixi, inr, sig, src, dst,
                        t->has_authority ? auth : NULL, amount};
    return put(s, "put_sol_transfer", 9, v, err);
}

static idx_status put_token_transfer(pg_store *s,
                                     const idx_store_transfer_row *r,
                                     idx_error *err) {
    const idx_transfer *t = &r->transfer;
    char slot[NUM_BUF], txi[NUM_BUF], ixi[NUM_BUF], inr[NUM_BUF], sig[SIG_BUF];
    char kind[NUM_BUF], src[PUBKEY_BUF], dst[PUBKEY_BUF], auth[PUBKEY_BUF];
    char mint[PUBKEY_BUF], sowner[PUBKEY_BUF], downer[PUBKEY_BUF];
    char amount[NUM_BUF], fee[NUM_BUF], decimals[NUM_BUF];
    event_path(&r->ref, t->instruction_index, t->inner_index, t->inner, slot,
               txi, ixi, inr, sig);
    u64_str(t->kind, kind);
    pubkey_hex(&t->source, src);
    pubkey_hex(&t->destination, dst);
    pubkey_hex(&t->authority, auth);
    pubkey_hex(&t->mint, mint);
    pubkey_hex(&t->source_owner, sowner);
    pubkey_hex(&t->destination_owner, downer);
    u64_str(t->amount, amount);
    u64_str(t->fee, fee);
    u64_str(t->decimals, decimals);
    const char *v[15] = {slot,
                         txi,
                         ixi,
                         inr,
                         sig,
                         kind,
                         src,
                         dst,
                         t->has_authority ? auth : NULL,
                         t->has_mint ? mint : NULL,
                         t->has_source_owner ? sowner : NULL,
                         t->has_destination_owner ? downer : NULL,
                         amount,
                         fee,
                         t->has_decimals ? decimals : NULL};
    return put(s, "put_token_transfer", 15, v, err);
}

static idx_status put_swap(pg_store *s, const idx_store_swap_row *r,
                           idx_error *err) {
    const idx_swap_row *sw = &r->swap;
    char slot[NUM_BUF], txi[NUM_BUF], ixi[NUM_BUF], inr[NUM_BUF], sig[SIG_BUF];
    char kind[NUM_BUF], venue[NUM_BUF], amtsrc[NUM_BUF], pool[PUBKEY_BUF];
    char trader[PUBKEY_BUF], imint[PUBKEY_BUF], iamt[NUM_BUF], idec[NUM_BUF];
    char omint[PUBKEY_BUF], oamt[NUM_BUF], odec[NUM_BUF], price[DBL_BUF];
    char quote[NUM_BUF], btime[NUM_BUF];
    event_path(&r->ref, sw->instruction_index, sw->inner_index, sw->inner, slot,
               txi, ixi, inr, sig);
    u64_str(sw->kind, kind);
    u64_str(sw->venue, venue);
    u64_str(sw->source, amtsrc);
    pubkey_hex(&sw->pool, pool);
    pubkey_hex(&sw->user, trader);
    pubkey_hex(&sw->input_mint, imint);
    u64_str(sw->input_amount, iamt);
    u64_str(sw->input_decimals, idec);
    pubkey_hex(&sw->output_mint, omint);
    u64_str(sw->output_amount, oamt);
    u64_str(sw->output_decimals, odec);
    dbl_str(r->price.price, price);
    u64_str(r->price.quote, quote);
    i64_str(r->block_time, btime);
    const char *v[19] = {slot,
                         txi,
                         ixi,
                         inr,
                         sig,
                         kind,
                         venue,
                         amtsrc,
                         sw->has_pool ? pool : NULL,
                         sw->has_user ? trader : NULL,
                         sw->has_input_mint ? imint : NULL,
                         sw->has_input_amount ? iamt : NULL,
                         sw->has_input_decimals ? idec : NULL,
                         sw->has_output_mint ? omint : NULL,
                         sw->has_output_amount ? oamt : NULL,
                         sw->has_output_decimals ? odec : NULL,
                         r->price.has_price ? price : NULL,
                         r->price.priced ? quote : NULL,
                         r->has_block_time ? btime : NULL};
    return put(s, "put_swap", 19, v, err);
}

static idx_status put_pool(pg_store *s, const idx_pool *p, idx_error *err) {
    char addr[PUBKEY_BUF], venue[NUM_BUF], ma[PUBKEY_BUF], da[NUM_BUF];
    char mb[PUBKEY_BUF], db[NUM_BUF], seen[NUM_BUF], swaps[NUM_BUF];
    char cslot[NUM_BUF], creator[PUBKEY_BUF];
    pubkey_hex(&p->address, addr);
    u64_str(p->venue, venue);
    pubkey_hex(&p->mint_a, ma);
    u64_str(p->decimals_a, da);
    pubkey_hex(&p->mint_b, mb);
    u64_str(p->decimals_b, db);
    u64_str(p->first_seen_slot, seen);
    u64_str(p->swap_count, swaps);
    u64_str(p->creation_slot, cslot);
    pubkey_hex(&p->creator, creator);
    const char *v[10] = {addr,
                         venue,
                         p->has_mint_a ? ma : NULL,
                         p->has_decimals_a ? da : NULL,
                         p->has_mint_b ? mb : NULL,
                         p->has_decimals_b ? db : NULL,
                         seen,
                         swaps,
                         p->has_creation ? cslot : NULL,
                         p->has_creator ? creator : NULL};
    return put(s, "put_pool", 10, v, err);
}

static idx_status put_token(pg_store *s, const idx_token *t, idx_error *err) {
    char mint[PUBKEY_BUF], decimals[NUM_BUF], seen[NUM_BUF];
    pubkey_hex(&t->mint, mint);
    u64_str(t->decimals, decimals);
    u64_str(t->first_seen_slot, seen);
    const char *v[6] = {mint,
                        t->has_decimals ? decimals : NULL,
                        t->has_name ? t->name : NULL,
                        t->has_symbol ? t->symbol : NULL,
                        t->has_uri ? t->uri : NULL,
                        seen};
    return put(s, "put_token", 6, v, err);
}

static idx_status put_bar(pg_store *s, const idx_bar *b, idx_error *err) {
    char pool[PUBKEY_BUF], interval[NUM_BUF], bucket[NUM_BUF], quote[NUM_BUF];
    char open[DBL_BUF], high[DBL_BUF], low[DBL_BUF], close[DBL_BUF];
    char bvol[DBL_BUF], qvol[DBL_BUF], swaps[NUM_BUF];
    char okey[SEQ_KEY_BUF], ckey[SEQ_KEY_BUF], cslot[NUM_BUF];
    pubkey_hex(&b->pool, pool);
    u64_str(b->interval, interval);
    i64_str(b->bucket, bucket);
    u64_str(b->quote, quote);
    dbl_str(b->open, open);
    dbl_str(b->high, high);
    dbl_str(b->low, low);
    dbl_str(b->close, close);
    dbl_str(b->base_volume, bvol);
    dbl_str(b->quote_volume, qvol);
    u64_str(b->swap_count, swaps);
    seq_key_hex(&b->open_seq, okey);
    seq_key_hex(&b->close_seq, ckey);
    u64_str(b->close_seq.slot, cslot);
    const char *v[14] = {pool, interval, bucket, quote, open,  high,  low,
                         close, bvol,    qvol,   swaps, okey, ckey, cslot};
    return put(s, "put_bar", 14, v, err);
}

/* Applies every row of `set` with no transaction of its own, returning the
 * first failure so the caller can roll its transaction back. */
static idx_status apply_write_set(pg_store *s, const idx_store_write_set *set,
                                  idx_error *err) {
    if (set == NULL) {
        return IDX_OK;
    }

#define WRITE_ALL(array, count, fn)                    \
    do {                                               \
        for (size_t i = 0; i < (set->count); i++) {    \
            IDX_TRY(fn(s, &set->array[i], err));        \
        }                                              \
    } while (0)

    WRITE_ALL(blocks, block_count, put_block);
    WRITE_ALL(sol_balances, sol_balance_count, put_sol_balance);
    WRITE_ALL(token_balances, token_balance_count, put_token_balance);
    /* Transfers split by kind (D5): SOL to one table, the rest to the other. */
    for (size_t i = 0; i < set->transfer_count; i++) {
        const idx_store_transfer_row *r = &set->transfers[i];
        IDX_TRY(r->transfer.kind == IDX_TRANSFER_SOL
                    ? put_sol_transfer(s, r, err)
                    : put_token_transfer(s, r, err));
    }
    WRITE_ALL(swaps, swap_count, put_swap);
    WRITE_ALL(pools, pool_count, put_pool);
    WRITE_ALL(tokens, token_count, put_token);
    WRITE_ALL(bars, bar_count, put_bar);
#undef WRITE_ALL

    return IDX_OK;
}

static idx_status store_write(void *ctx, const idx_store_write_set *set,
                              idx_error *err) {
    pg_store *s = ctx;
    IDX_TRY(idx_pg_begin(s->conn, err));
    idx_status st = apply_write_set(s, set, err);
    if (st != IDX_OK) {
        idx_pg_rollback(s->conn, NULL);
        return st;
    }
    return idx_pg_commit(s->conn, err);
}

/*
 * Reorg in one transaction (D4): delete every row at or above `from_slot`, then
 * apply the caller's replacement — which already carries the bars it recomputed
 * for the affected buckets (store.h) — so a reader never sees a half-applied
 * reorg. A NULL replacement is a pure delete.
 */
static idx_status store_reorg(void *ctx, idx_slot from_slot,
                              const idx_store_write_set *replacement,
                              idx_error *err) {
    pg_store *s = ctx;
    char slot[NUM_BUF];
    u64_str(from_slot, slot);
    const char *p[1] = {slot};

    IDX_TRY(idx_pg_begin(s->conn, err));
    for (size_t i = 0; i < sizeof g_reorg_deletes / sizeof g_reorg_deletes[0];
         i++) {
        idx_status st =
            idx_pg_exec_prepared(s->conn, g_reorg_deletes[i], 1, p, NULL, err);
        if (st != IDX_OK) {
            idx_pg_rollback(s->conn, NULL);
            return st;
        }
    }
    idx_status st = apply_write_set(s, replacement, err);
    if (st != IDX_OK) {
        idx_pg_rollback(s->conn, NULL);
        return st;
    }
    return idx_pg_commit(s->conn, err);
}

static void store_close(void *ctx) {
    pg_store *s = ctx;
    if (s != NULL) {
        idx_pg_close(s->conn);
        free(s);
    }
}

static const idx_confirmed_store_vt g_vt = {
    .name = store_name,
    .write = store_write,
    .reorg = store_reorg,
    /* prune (retention) and read_range (promotion) are their own M7 items;
     * until then the store.h dispatch reports them unsupported rather than
     * misbehaving. */
    .close = store_close,
};

idx_status idx_pg_confirmed_store_open(const char *conninfo,
                                       idx_confirmed_store **out,
                                       idx_error *err) {
    if (conninfo == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG,
                        "pg confirmed store: null argument");
    }
    idx_pg_conn *conn = NULL;
    IDX_TRY(idx_pg_connect(conninfo, &conn, err));

    for (size_t i = 0; g_schema[i] != NULL; i++) {
        idx_status st = idx_pg_exec(conn, g_schema[i], NULL, err);
        if (st != IDX_OK) {
            idx_pg_close(conn);
            return st;
        }
    }
    for (size_t i = 0; i < STMT_COUNT; i++) {
        idx_status st = idx_pg_prepare(conn, g_stmts[i].name, g_stmts[i].sql,
                                       g_stmts[i].n_params, err);
        if (st != IDX_OK) {
            idx_pg_close(conn);
            return st;
        }
    }

    idx_confirmed_store *store = calloc(1, sizeof *store);
    pg_store *s = calloc(1, sizeof *s);
    if (store == NULL || s == NULL) {
        free(store);
        free(s);
        idx_pg_close(conn);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "pg confirmed store: alloc");
    }
    s->conn = conn;
    store->vt = &g_vt;
    store->ctx = s;
    *out = store;
    return IDX_OK;
}

#else /* !IDX_HAVE_LIBPQ */

typedef int idx_pg_store_translation_unit_not_empty;

#endif /* IDX_HAVE_LIBPQ */
