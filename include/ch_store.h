/*
 * Finalized-tier store backed by ClickHouse (ROADMAP.md milestone M7, D4/D5).
 *
 * The finalized half of the storage abstraction (store.h), over the HTTP client
 * (ch.h) and the row serializer (ch_rows.h). It owns the schema — the D5
 * entities as denormalized tables — and turns an idx_store_write_set into
 * per-table row bodies.
 *
 * What makes this tier a different shape from the confirmed one (D4):
 *
 *   - It is append-only. A reorg can never reach finalized data, so there is no
 *     delete, no reorg and no read-back — just append and flush.
 *   - It has no upserts, because ClickHouse has none. Every table is a
 *     ReplacingMergeTree keyed on its sort key with a version column, so
 *     re-inserting a row converges instead of duplicating (§5.5 of
 *     docs/specification/STORAGE.md). Deduplication happens at *merge* time,
 *     which means a reader either tolerates duplicates or uses FINAL. That is a
 *     real constraint on the query layer (M9), not a footnote.
 *   - Bars are one table per resolution (bars_1s, bars_1m, bars_1d) rather than
 *     one table keyed by interval, because the retention and rollup policies
 *     genuinely differ per resolution. bars_1d is created here and written by
 *     the rollup, a later item.
 *
 * The version column per table, since it is what "re-indexing is safe" means
 * here:
 *
 *   blocks, events         the slot. The sort key already identifies the row, so
 *                          a re-index writes an identical one and the version
 *                          only makes the dedup explicit.
 *   balance state          the slot. The latest observation of an account wins
 *                          without a delete, which is the whole reason this tier
 *                          can hold state at all.
 *   pools, tokens          `version`, the highest slot in the write set that
 *                          carried the record. A dimension accumulates in the
 *                          registry and arrives here already merged, so the most
 *                          recently written record is the complete one. It is a
 *                          column of its own because nothing else on a dimension
 *                          row is monotonic — first_seen_slot is fixed, and a
 *                          token gains a name without gaining anything countable.
 *   bars                   `swap_count`. A bar is a fold, so a later write of the
 *                          same bucket has folded at least as many swaps as an
 *                          earlier one; the count is exactly the fold's progress.
 *
 * `append` buffers and `flush` writes (D3): every insert creates a part, and one
 * insert per block is what TOO_MANY_PARTS looks like at this rate. Nothing is
 * durable until a flush. *When* to flush — the row-count and time bounds — is
 * the batching writer's, a later item; this exposes the bounds to read and does
 * nothing on its own.
 *
 * A flush is not atomic across tables: ClickHouse has no multi-table
 * transaction, so a failure part-way leaves the tables before it written. That
 * is safe rather than merely tolerable, because every table is idempotent under
 * re-insert — the caller retries the same flush and the rows that landed twice
 * collapse on merge. A failed flush keeps the rows it did not write buffered.
 *
 * Not thread-safe: one connection, on the processing thread (D6).
 */
#ifndef IDX_CH_STORE_H
#define IDX_CH_STORE_H

#include <stddef.h>

#include "ch.h"
#include "ch_rows.h"
#include "error.h"
#include "store.h"

/*
 * Opens a finalized store on `options` (see ch.h), creating the schema if it is
 * absent. `format` selects the insert format for every table: RowBinary is the
 * hot path, JSONEachRow is for development and debugging, and the two produce
 * the same rows (ch_rows.h). The returned handle is an idx_finalized_store like
 * any other — close it with idx_finalized_store_close, which drops the
 * connection. Closing does *not* flush: buffered rows are lost, which is
 * deliberate, since a store being torn down after an error has no one to report
 * a failed insert to. Flush before closing.
 *
 *   IDX_OK               `*out` owns the store
 *   IDX_ERR_INVALID_ARG  options, its url, or out is NULL
 *   IDX_ERR_NETWORK      the server could not be reached
 *   IDX_ERR_REMOTE       the schema was rejected
 *   IDX_ERR_NO_MEMORY    allocation failed
 */
idx_status idx_ch_finalized_store_open(const idx_ch_options *options,
                                       idx_ch_row_format format,
                                       idx_finalized_store **out,
                                       idx_error *err);

/*
 * The DDL idx_ch_finalized_store_open applies, as a NULL-terminated array of
 * statements. Exposed so migration tooling and the tests can create or inspect
 * the schema without opening a store. Never NULL.
 */
const char *const *idx_ch_finalized_schema(void);

/* Rows buffered and not yet flushed, across every table. What the batching
 * writer's row-count bound reads. Zero for any handle this module did not
 * return. */
size_t idx_ch_finalized_store_pending(const idx_finalized_store *store);

/* Bytes those buffered rows occupy, across every table. The other bound a
 * batching writer may want: rows differ in width by an order of magnitude
 * between a balance and a swap. */
size_t idx_ch_finalized_store_pending_bytes(const idx_finalized_store *store);

#endif /* IDX_CH_STORE_H */
