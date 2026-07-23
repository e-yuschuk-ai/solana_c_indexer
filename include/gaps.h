/*
 * The set of slots known to be missing and not yet recovered
 * (ROADMAP.md milestone M4).
 *
 * Holes arrive from three places and are all the same thing once here: slots
 * the socket never delivered because it reconnected, slots the ring dropped
 * because the processing thread was behind (decision D6), and the stretch
 * between a resumed cursor and the chain tip.
 *
 * This is also where the indexer's durable position comes from. Committing the
 * highest slot seen is not the same as having indexed everything below it, and
 * with fetchers filling old holes while live blocks keep arriving the two
 * diverge constantly. idx_gaps_watermark answers the question that actually
 * matters — the highest slot with nothing outstanding beneath it — so a restart
 * resumes at a point it can trust rather than at a high-water mark that skips
 * whatever was still in flight.
 *
 * Ranges are inclusive on both ends, kept sorted and disjoint. The structure
 * holds slot numbers only, never blocks, so it stays small even when a long
 * outage leaves millions of slots outstanding.
 *
 * Thread-safe: the processing thread adds, the fetchers claim and resolve.
 */
#ifndef IDX_GAPS_H
#define IDX_GAPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "types.h"

typedef struct idx_gaps idx_gaps;

/*
 * Ranges the set will track before it starts coarsening. Reaching this means
 * something is badly wrong upstream — an endpoint down for hours, say — and
 * the response is to merge the two closest ranges rather than to grow without
 * bound or to forget a hole. Merging swallows the slots between them, so the
 * set stays a superset of what is really missing: the cost is refetching slots
 * that were already indexed, which is idempotent, and never silently losing
 * one.
 */
#define IDX_GAPS_DEFAULT_MAX_RANGES 4096u

typedef struct {
    idx_slot from; /* inclusive */
    idx_slot to;   /* inclusive */
} idx_gap_range;

typedef struct {
    uint64_t added;      /* ranges recorded */
    uint64_t resolved;   /* slots removed as done or known-absent */
    uint64_t coarsened;  /* merges forced by the range limit */
    size_t range_count;  /* ranges currently held */
    uint64_t slot_count; /* slots currently outstanding */
    idx_slot lowest;     /* IDX_SLOT_NONE when nothing is outstanding */
} idx_gaps_stats;

idx_status idx_gaps_new(size_t max_ranges, idx_gaps **out, idx_error *err);
void idx_gaps_free(idx_gaps *gaps);

/*
 * Records [from, to] as missing. Merges with anything it touches. An empty or
 * inverted range is ignored rather than rejected: callers derive these from
 * slot arithmetic and "nothing was missing" is a normal outcome.
 */
idx_status idx_gaps_add(idx_gaps *gaps, idx_slot from, idx_slot to,
                        idx_error *err);

/*
 * Claims the lowest outstanding range, narrowed to at most `max_span` slots,
 * for a fetcher to work on. A claimed range is invisible to other claimants but
 * still counts against the watermark, because it is not recovered yet.
 *
 * Returns false when nothing is claimable. The caller must finish every claim
 * with idx_gaps_resolve, idx_gaps_release, or both.
 */
bool idx_gaps_claim(idx_gaps *gaps, uint64_t max_span, idx_gap_range *out);

/*
 * Drops [from, to] from the set: those slots are indexed, or the chain never
 * produced them, or the endpoint no longer retains them. Either way there is
 * nothing left to fetch.
 */
void idx_gaps_resolve(idx_gaps *gaps, idx_slot from, idx_slot to);

/*
 * Returns [from, to] to the claimable set after a fetch failed part way. Used
 * for the tail of a claim the fetcher could not finish, so another attempt
 * picks it up instead of the range being lost.
 */
idx_status idx_gaps_release(idx_gaps *gaps, idx_slot from, idx_slot to,
                            idx_error *err);

/*
 * The highest slot with nothing outstanding at or below it, which is what the
 * cursor should persist.
 *
 * `highest_committed` is the highest slot the pipeline has handed to storage,
 * or IDX_SLOT_NONE if none. With no outstanding gaps that is the answer. With
 * gaps, the answer is one below the lowest outstanding slot — everything from
 * there up is unreliable, however much of it has been committed.
 *
 * Returns IDX_SLOT_NONE when nothing can be trusted yet.
 */
idx_slot idx_gaps_watermark(const idx_gaps *gaps, idx_slot highest_committed);

/* True when nothing is outstanding, claimed or otherwise. */
bool idx_gaps_is_empty(const idx_gaps *gaps);

void idx_gaps_get_stats(const idx_gaps *gaps, idx_gaps_stats *out);

#endif /* IDX_GAPS_H */
