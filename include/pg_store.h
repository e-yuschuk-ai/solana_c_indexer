/*
 * Confirmed-tier store backed by PostgreSQL (ROADMAP.md milestone M7, D4/D5).
 *
 * This is the confirmed half of the storage abstraction (store.h) implemented
 * over the libpq client (pg.h). It owns the schema — the D5 entities as tables
 * indexed by slot — and turns an idx_store_write_set into SQL:
 *
 *   - blocks and the pool/token dimensions and the bars are upserted on their
 *     natural key, so re-indexing a slot converges rather than duplicating;
 *   - the transfer and swap events are keyed on the instruction path
 *     (slot, transaction_index, instruction_index, inner_index), which D5 makes
 *     unique, so they too are idempotent;
 *   - balance state is upserted on the account (D5): the tier holds one current
 *     value per account, not a log;
 *   - bars merge on conflict — open/close by execution order, high/low/volume
 *     accumulated — so a bucket that spans several write calls is folded, not
 *     overwritten.
 *
 * Every table carries `slot` and is indexed by it, which is what makes the
 * reorg delete and the retention prune the cheap range operations D4 wants.
 * Reorg runs in one transaction (D4): delete at or above the reorged slot, then
 * apply the caller's replacement (which already carries the bars it recomputed,
 * per store.h), so a reader never sees a half-applied reorg.
 *
 * uint64 amounts that can exceed int64 (raw token amounts — see balance.h) are
 * stored as NUMERIC(20,0); keys, hashes and signatures as BYTEA. The schema is
 * created on open with CREATE TABLE IF NOT EXISTS, so opening against a fresh
 * database just works; a dedicated migration path is a later M7 item.
 *
 * Not thread-safe: it wraps one connection and belongs to the processing
 * thread (D6).
 */
#ifndef IDX_PG_STORE_H
#define IDX_PG_STORE_H

#include "error.h"
#include "store.h"

/*
 * Opens a confirmed store on `conninfo` (a libpq DSN; see pg.h), creating the
 * schema if it is absent and preparing the statements the write path uses. The
 * returned handle is an idx_confirmed_store like any other — close it with
 * idx_confirmed_store_close, which drops the connection.
 *
 *   IDX_OK             `*out` owns the store
 *   IDX_ERR_INVALID_ARG  conninfo or out is NULL
 *   IDX_ERR_NETWORK    the database could not be reached
 *   IDX_ERR_REMOTE     the schema or a prepared statement was rejected
 *   IDX_ERR_NO_MEMORY  allocation failed
 */
idx_status idx_pg_confirmed_store_open(const char *conninfo,
                                       idx_confirmed_store **out,
                                       idx_error *err);

/*
 * The DDL that idx_pg_confirmed_store_open applies, as a NULL-terminated array
 * of statements. Exposed so the migration tooling and the tests can create or
 * inspect the schema without opening a store. Never NULL.
 */
const char *const *idx_pg_confirmed_schema(void);

#endif /* IDX_PG_STORE_H */
