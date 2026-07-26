/*
 * Storage abstraction layer (ROADMAP.md milestone M7, decision D4).
 *
 * The indexer persists to two tiers, and they are not the same shape (D4):
 *
 *   confirmed  PostgreSQL, mutable. Holds the unfinalized window (~13 slots).
 *              A reorg overwrites it: delete at or above the reorged slot and
 *              rewrite. It is also the source promotion reads from.
 *   finalized  ClickHouse, append-only. A reorg can never reach it, so it never
 *              deletes; writes are batched and flushed.
 *
 * Forcing both behind one identical interface would either give the finalized
 * tier operations it must never perform (delete, reorg) or deny the confirmed
 * tier the ones it exists for. So this defines *one interface per tier* — two
 * vtables — over a single shared vocabulary of what gets written: the D5
 * entities, gathered into an idx_store_write_set.
 *
 * This is the seam, not a backend. The libpq and ClickHouse clients (the next
 * M7 items) provide the vtables; the pipeline holds only these handles and
 * never sees either database. An in-memory reference implementation of each
 * tier lives here too — it is what a real backend is contract-tested against,
 * and it lets the pipeline run end to end with no database attached.
 *
 * A store handle belongs to one thread; the write path runs on the processing
 * thread (D6). Nothing here is thread-safe.
 */
#ifndef IDX_STORE_H
#define IDX_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "balance.h"
#include "bar.h"
#include "error.h"
#include "pool.h"
#include "price.h"
#include "swap.h"
#include "token.h"
#include "transfer.h"
#include "types.h"

/* ---------------------------------------------------------- write vocabulary -- */

/*
 * The part of D5's instruction path an event row does not carry on its own. The
 * entity structs hold their position within a transaction (instruction and
 * inner index); the slot and transaction index are the caller's, and the
 * signature links the row back to an explorer since no transaction table does
 * (D5). Every row that is deleted by slot on the reorg path carries this so the
 * store can find it.
 */
typedef struct {
    idx_slot slot;
    uint16_t transaction_index;
    idx_signature signature;
} idx_store_ref;

/*
 * A block header (D5 `blocks`), keyed by slot. The transactions themselves are
 * not stored — the entities derived from them are — so only the header travels,
 * plus the transaction count as a convenience for a consumer.
 */
typedef struct {
    idx_slot slot;
    idx_hash blockhash;
    idx_hash previous_blockhash;
    idx_slot parent_slot;
    int64_t block_time; /* unix seconds; valid only when has_block_time */
    bool has_block_time;
    uint64_t block_height; /* valid only when has_block_height */
    bool has_block_height;
    uint32_t transaction_count;
} idx_store_block_row;

/* An SOL balance observation (D5 `sol_balances`, state keyed by account). */
typedef struct {
    idx_store_ref ref;
    idx_sol_balance balance;
} idx_store_sol_balance_row;

/* A token balance observation (D5 `token_balances`, state keyed by account). */
typedef struct {
    idx_store_ref ref;
    idx_token_balance_state balance;
} idx_store_token_balance_row;

/* A transfer event (D5 `sol_transfers`/`token_transfers`; `transfer.kind`
 * decides which table a tier routes it to). */
typedef struct {
    idx_store_ref ref;
    idx_transfer transfer;
} idx_store_transfer_row;

/*
 * A swap event (D5 `swaps`), with its price alongside. Price is a nullable
 * column on the swap, not a table (D5): `price.has_price` says whether a number
 * was computed. `block_time` is the block's timestamp, carried so the tier can
 * bucket bars without a join back to the block.
 */
typedef struct {
    idx_store_ref ref;
    idx_swap_row swap;
    idx_price price;
    int64_t block_time;
    bool has_block_time;
} idx_store_swap_row;

/*
 * One unit of work handed to a store: the entities derived from one or more
 * slots, every array borrowed and valid only for the duration of the call. The
 * dimensions (pools, tokens) and bars are already-merged registry records; the
 * events and balances are per-transaction rows carrying their idx_store_ref.
 * Any array may be empty.
 */
typedef struct {
    const idx_store_block_row *blocks;
    size_t block_count;

    const idx_store_sol_balance_row *sol_balances;
    size_t sol_balance_count;

    const idx_store_token_balance_row *token_balances;
    size_t token_balance_count;

    const idx_store_transfer_row *transfers;
    size_t transfer_count;

    const idx_store_swap_row *swaps;
    size_t swap_count;

    const idx_pool *pools;
    size_t pool_count;

    const idx_token *tokens;
    size_t token_count;

    const idx_bar *bars;
    size_t bar_count;
} idx_store_write_set;

/* Zeroes `set` to the empty write set. */
void idx_store_write_set_init(idx_store_write_set *set);

/* Rows across every entity in the set. */
size_t idx_store_write_set_total(const idx_store_write_set *set);

/* Row counts per entity, for observability and for the tests that assert the
 * reference store's contents. */
typedef struct {
    size_t blocks;
    size_t sol_balances;
    size_t token_balances;
    size_t transfers;
    size_t swaps;
    size_t pools;
    size_t tokens;
    size_t bars;
} idx_store_counts;

/* Rows across every entity in the counts. */
size_t idx_store_counts_total(const idx_store_counts *counts);

/* ----------------------------------------------------- confirmed tier (D4) -- */

typedef struct idx_confirmed_store idx_confirmed_store;

/*
 * The confirmed tier's operations. A backend fills this in and pairs it with
 * its own context. Every function may assume its handle-level arguments were
 * null-checked by the dispatch wrappers below.
 */
typedef struct {
    /* Short tier/backend name for logs; never NULL. */
    const char *(*name)(void *ctx);

    /* Persists `set`. One atomic unit of work (D4): a reader never sees it half
     * applied. */
    idx_status (*write)(void *ctx, const idx_store_write_set *set,
                        idx_error *err);

    /*
     * Reorg (D4), atomically: delete every row at or above `from_slot`, then
     * apply `replacement` — the corrected rows for those slots, which the caller
     * derived, including the bars it recomputed for the affected buckets
     * (idx_bar_registry_recompute_range). `replacement` may be NULL to delete
     * without rewriting.
     */
    idx_status (*reorg)(void *ctx, idx_slot from_slot,
                        const idx_store_write_set *replacement, idx_error *err);

    /* Retention (D4): drop every row below `below_slot`, once promoted and past
     * the safety margin the caller enforces by its choice of slot. */
    idx_status (*prune)(void *ctx, idx_slot below_slot, idx_error *err);

    /*
     * Promotion source (D4): gather every row in the closed slot range
     * [`from_slot`, `to_slot`] into `*out`, allocated from `arena` so it
     * outlives nothing of the store's. This is the bulk read that feeds the
     * finalized tier's append path without refetching the block.
     */
    idx_status (*read_range)(void *ctx, idx_slot from_slot, idx_slot to_slot,
                             idx_arena *arena, idx_store_write_set *out,
                             idx_error *err);

    /* Releases the backend. The handle is freed by the dispatch wrapper. */
    void (*close)(void *ctx);
} idx_confirmed_store_vt;

struct idx_confirmed_store {
    const idx_confirmed_store_vt *vt;
    void *ctx;
};

/* Backend name, or "unset" for a NULL handle. Never NULL. */
const char *idx_confirmed_store_name(const idx_confirmed_store *store);

/* Dispatch wrappers. Each reports IDX_ERR_INVALID_ARG when the handle, its
 * vtable, or the operation it needs is NULL, and otherwise forwards. */
idx_status idx_confirmed_store_write(idx_confirmed_store *store,
                                     const idx_store_write_set *set,
                                     idx_error *err);
idx_status idx_confirmed_store_reorg(idx_confirmed_store *store,
                                     idx_slot from_slot,
                                     const idx_store_write_set *replacement,
                                     idx_error *err);
idx_status idx_confirmed_store_prune(idx_confirmed_store *store,
                                     idx_slot below_slot, idx_error *err);
idx_status idx_confirmed_store_read_range(idx_confirmed_store *store,
                                          idx_slot from_slot, idx_slot to_slot,
                                          idx_arena *arena,
                                          idx_store_write_set *out,
                                          idx_error *err);

/* Closes and frees the handle. Safe to call with NULL. */
void idx_confirmed_store_close(idx_confirmed_store *store);

/* ----------------------------------------------------- finalized tier (D4) -- */

typedef struct idx_finalized_store idx_finalized_store;

/*
 * The finalized tier's operations. Append-only: there is no delete, no reorg
 * and no read-back, because a reorg can never reach it and promotion only ever
 * writes into it.
 */
typedef struct {
    const char *(*name)(void *ctx);

    /* Buffers `set` for the batching writer. Nothing is guaranteed durable
     * until flush (D3): one insert per block is what this exists to avoid. */
    idx_status (*append)(void *ctx, const idx_store_write_set *set,
                         idx_error *err);

    /* Flushes the buffered rows. Called on a row-count or time bound, and once
     * more at shutdown. */
    idx_status (*flush)(void *ctx, idx_error *err);

    void (*close)(void *ctx);
} idx_finalized_store_vt;

struct idx_finalized_store {
    const idx_finalized_store_vt *vt;
    void *ctx;
};

const char *idx_finalized_store_name(const idx_finalized_store *store);

idx_status idx_finalized_store_append(idx_finalized_store *store,
                                      const idx_store_write_set *set,
                                      idx_error *err);
idx_status idx_finalized_store_flush(idx_finalized_store *store, idx_error *err);

void idx_finalized_store_close(idx_finalized_store *store);

/* ------------------------------------------- in-memory reference backends -- */

/*
 * Row-oriented reference implementations of each tier. They keep every row they
 * are given and implement the slot-ranged delete, rewrite and read the
 * interface promises, so a real backend can be checked against them and the
 * pipeline can run with no database. What they deliberately do *not* model is
 * the schema-level shape of a specific tier — the confirmed tier's upsert by
 * account, the finalized tier's ReplacingMergeTree dedup — which belongs to the
 * schema items that build each backend, not to the abstraction.
 *
 *   IDX_OK             `*out` owns a handle; close it with the matching _close
 *   IDX_ERR_NO_MEMORY  allocation failed
 */
idx_status idx_mem_confirmed_store_open(idx_confirmed_store **out,
                                        idx_error *err);
idx_status idx_mem_finalized_store_open(idx_finalized_store **out,
                                        idx_error *err);

/*
 * Rows the reference confirmed store currently holds, per entity. `store` must
 * be one returned by idx_mem_confirmed_store_open; the counts are zero for any
 * other handle.
 */
void idx_mem_confirmed_store_counts(const idx_confirmed_store *store,
                                    idx_store_counts *out);

/* Rows the reference finalized store holds in total (buffered plus flushed). */
void idx_mem_finalized_store_counts(const idx_finalized_store *store,
                                    idx_store_counts *out);

/* Rows appended but not yet flushed on the reference finalized store. */
size_t idx_mem_finalized_store_pending(const idx_finalized_store *store);

/* How many times flush has been called on the reference finalized store. */
size_t idx_mem_finalized_store_flushes(const idx_finalized_store *store);

#endif /* IDX_STORE_H */
