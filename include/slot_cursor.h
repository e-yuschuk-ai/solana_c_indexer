/*
 * Slot cursor: the indexer's position on the chain.
 *
 * (Named to stay clear of idx_cursor in bytes.h, which walks a byte buffer and
 * is a different thing entirely.)
 *
 * Two frontiers, tracked separately because they answer different questions:
 *
 *   last_indexed  the highest slot fully committed to storage. Durable —
 *                 idx_slot_cursor_save writes it and idx_slot_cursor_open
 *                 reloads it — so a restart resumes where indexing stopped
 *                 rather than at the tip.
 *   last_seen     the highest slot the socket delivered this run. Kept in
 *                 memory only; it is never reset on a reconnect, which is what
 *                 lets the range missed while the socket was down be replayed:
 *                 the first notification after the drop, compared against
 *                 last_seen, exposes exactly the hole.
 *
 * The cursor is state plus persistence. Deciding that a distance between a
 * frontier and a notified slot is a gap, and fetching that gap, belong to the
 * ingestion pipeline (ROADMAP.md milestone M4); the cursor only records the
 * facts those decisions are made from.
 *
 * Not thread-safe; one owner per instance.
 */
#ifndef IDX_SLOT_CURSOR_H
#define IDX_SLOT_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "error.h"
#include "types.h"

#define IDX_SLOT_CURSOR_PATH_MAX 512

/*
 * Sentinel for "no slot recorded". Slot 0 is genesis, a real slot, so it
 * cannot double as the empty value; the maximum, which the chain will not
 * reach, can.
 */
#define IDX_SLOT_NONE UINT64_MAX

typedef struct {
    idx_slot last_indexed; /* durable; IDX_SLOT_NONE before anything is indexed */
    idx_slot last_seen;    /* runtime; IDX_SLOT_NONE before the first notification */

    /*
     * Floor for a fresh start; 0 means "from the current chain tip". Consulted
     * only when last_indexed is IDX_SLOT_NONE — once there is progress to
     * resume from, the persisted position wins.
     */
    idx_slot start_slot;

    /* Where idx_slot_cursor_save writes. Empty for an in-memory cursor. */
    char path[IDX_SLOT_CURSOR_PATH_MAX];
} idx_slot_cursor;

/* In-memory cursor with no persistence. Both frontiers start at IDX_SLOT_NONE. */
void idx_slot_cursor_init(idx_slot_cursor *cursor, idx_slot start_slot);

/*
 * Cursor backed by `path`. When the file exists its last_indexed is loaded, so
 * indexing resumes where it stopped; a missing file is a fresh start, not an
 * error. `path` may be NULL or empty for an in-memory cursor. Returns
 * IDX_ERR_PARSE when the file exists but is malformed.
 */
idx_status idx_slot_cursor_open(idx_slot_cursor *cursor, const char *path,
                                idx_slot start_slot, idx_error *err);

/*
 * Reads cursor state from `path` into `cursor->last_indexed`, leaving the other
 * fields untouched.
 *
 *   IDX_OK            last_indexed is updated (IDX_SLOT_NONE if the file
 *                     records no progress yet)
 *   IDX_ERR_NOT_FOUND the file does not exist; the caller decides whether that
 *                     is fatal
 *   IDX_ERR_IO        the file could not be read
 *   IDX_ERR_PARSE     the file exists but is malformed
 *
 * On failure `cursor` is left unchanged.
 */
idx_status idx_slot_cursor_load(idx_slot_cursor *cursor, const char *path,
                                idx_error *err);

/*
 * Persists the cursor to its `path` by writing a temporary file, flushing it to
 * disk and renaming it into place, so a crash mid-write cannot corrupt the
 * existing state. A no-op returning IDX_OK when the cursor has no path.
 */
idx_status idx_slot_cursor_save(const idx_slot_cursor *cursor, idx_error *err);

/*
 * Advances last_indexed to include `slot`. Monotonic: a slot at or below the
 * current position is ignored, so replaying an already-committed slot is
 * harmless. Does not persist — the caller controls save cadence.
 */
void idx_slot_cursor_record_indexed(idx_slot_cursor *cursor, idx_slot slot);

/*
 * Advances last_seen to include `slot`. Monotonic. This is the record that,
 * surviving a reconnect, exposes the range the socket missed.
 */
void idx_slot_cursor_observe(idx_slot_cursor *cursor, idx_slot slot);

/*
 * The slot to begin indexing at: one past last_indexed when resuming from
 * persisted state, otherwise start_slot. A return of 0 means "no floor — begin
 * from the current chain tip", which the caller resolves with getSlot.
 */
idx_slot idx_slot_cursor_resume_slot(const idx_slot_cursor *cursor);

#endif /* IDX_SLOT_CURSOR_H */
