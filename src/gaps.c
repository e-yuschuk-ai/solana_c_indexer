#include "gaps.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

typedef struct {
    idx_slot from;
    idx_slot to;
    bool claimed; /* a fetcher is working on it */
} idx_gaps_entry;

struct idx_gaps {
    pthread_mutex_t lock;

    idx_gaps_entry *ranges; /* sorted by `from`, disjoint */
    size_t count;
    size_t capacity;
    size_t max_ranges;

    idx_gaps_stats stats;
};

/* Caller holds the lock. */
static uint64_t slots_in(const idx_gaps *gaps) {
    uint64_t total = 0;
    for (size_t i = 0; i < gaps->count; i++) {
        total += gaps->ranges[i].to - gaps->ranges[i].from + 1;
    }
    return total;
}

/* Caller holds the lock. */
static void refresh_stats(idx_gaps *gaps) {
    gaps->stats.range_count = gaps->count;
    gaps->stats.slot_count = slots_in(gaps);
    gaps->stats.lowest =
        (gaps->count > 0) ? gaps->ranges[0].from : IDX_SLOT_NONE;
}

idx_status idx_gaps_new(size_t max_ranges, idx_gaps **out, idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null output parameter");
    }

    idx_gaps *gaps = calloc(1, sizeof(*gaps));
    if (gaps == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "out of memory");
    }
    gaps->max_ranges =
        (max_ranges != 0) ? max_ranges : (size_t)IDX_GAPS_DEFAULT_MAX_RANGES;
    gaps->stats.lowest = IDX_SLOT_NONE;
    pthread_mutex_init(&gaps->lock, NULL);

    *out = gaps;
    return IDX_OK;
}

void idx_gaps_free(idx_gaps *gaps) {
    if (gaps == NULL) {
        return;
    }
    pthread_mutex_destroy(&gaps->lock);
    free(gaps->ranges);
    free(gaps);
}

/* Caller holds the lock. */
static idx_status reserve_one(idx_gaps *gaps, idx_error *err) {
    if (gaps->count < gaps->capacity) {
        return IDX_OK;
    }
    size_t capacity = (gaps->capacity == 0) ? 16 : gaps->capacity * 2;
    idx_gaps_entry *grown =
        realloc(gaps->ranges, capacity * sizeof(*gaps->ranges));
    if (grown == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "out of memory");
    }
    gaps->ranges = grown;
    gaps->capacity = capacity;
    return IDX_OK;
}

/* Caller holds the lock. Removes the range at `index`. */
static void remove_at(idx_gaps *gaps, size_t index) {
    memmove(&gaps->ranges[index], &gaps->ranges[index + 1],
            (gaps->count - index - 1) * sizeof(*gaps->ranges));
    gaps->count--;
}

/*
 * Caller holds the lock. Merges unclaimed neighbours that now touch or overlap.
 * Claimed ranges are left alone: a fetcher is working from their bounds.
 */
static void coalesce(idx_gaps *gaps) {
    for (size_t i = 0; i + 1 < gaps->count;) {
        idx_gaps_entry *a = &gaps->ranges[i];
        idx_gaps_entry *b = &gaps->ranges[i + 1];

        bool adjacent = (a->to != IDX_SLOT_NONE) && (a->to + 1 >= b->from);
        if (!a->claimed && !b->claimed && adjacent) {
            if (b->to > a->to) {
                a->to = b->to;
            }
            remove_at(gaps, i + 1);
            continue;
        }
        i++;
    }
}

/*
 * Caller holds the lock. Brings the range count back under the limit by merging
 * the two closest unclaimed neighbours, which swallows the indexed slots
 * between them. Refetching those is idempotent; forgetting a hole would not be.
 */
static void coarsen(idx_gaps *gaps) {
    while (gaps->count > gaps->max_ranges) {
        size_t best = SIZE_MAX;
        idx_slot best_distance = IDX_SLOT_NONE;

        for (size_t i = 0; i + 1 < gaps->count; i++) {
            if (gaps->ranges[i].claimed || gaps->ranges[i + 1].claimed) {
                continue;
            }
            idx_slot distance = gaps->ranges[i + 1].from - gaps->ranges[i].to;
            if (best == SIZE_MAX || distance < best_distance) {
                best = i;
                best_distance = distance;
            }
        }
        if (best == SIZE_MAX) {
            return; /* everything is claimed; the limit has to give */
        }

        IDX_DEBUG("gaps: coarsening %llu..%llu into %llu..%llu",
                  (unsigned long long)gaps->ranges[best].from,
                  (unsigned long long)gaps->ranges[best].to,
                  (unsigned long long)gaps->ranges[best + 1].from,
                  (unsigned long long)gaps->ranges[best + 1].to);

        gaps->ranges[best].to = gaps->ranges[best + 1].to;
        remove_at(gaps, best + 1);
        gaps->stats.coarsened++;
    }
}

/* Caller holds the lock. Inserts [from, to] keeping the array sorted. */
static idx_status insert(idx_gaps *gaps, idx_slot from, idx_slot to,
                         idx_error *err) {
    IDX_TRY(reserve_one(gaps, err));

    size_t at = 0;
    while (at < gaps->count && gaps->ranges[at].from < from) {
        at++;
    }
    memmove(&gaps->ranges[at + 1], &gaps->ranges[at],
            (gaps->count - at) * sizeof(*gaps->ranges));
    gaps->ranges[at].from = from;
    gaps->ranges[at].to = to;
    gaps->ranges[at].claimed = false;
    gaps->count++;

    coalesce(gaps);
    coarsen(gaps);
    refresh_stats(gaps);
    return IDX_OK;
}

idx_status idx_gaps_add(idx_gaps *gaps, idx_slot from, idx_slot to,
                        idx_error *err) {
    if (gaps == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null gaps");
    }
    /* Callers derive these from slot arithmetic, where "nothing was missing"
     * is the common case rather than a mistake. */
    if (to < from || from == IDX_SLOT_NONE) {
        return IDX_OK;
    }

    pthread_mutex_lock(&gaps->lock);
    idx_status status = insert(gaps, from, to, err);
    if (status == IDX_OK) {
        gaps->stats.added++;
    }
    pthread_mutex_unlock(&gaps->lock);
    return status;
}

bool idx_gaps_claim(idx_gaps *gaps, uint64_t max_span, idx_gap_range *out) {
    if (gaps == NULL || out == NULL) {
        return false;
    }
    if (max_span == 0) {
        max_span = 1;
    }

    pthread_mutex_lock(&gaps->lock);

    bool found = false;
    for (size_t i = 0; i < gaps->count; i++) {
        if (gaps->ranges[i].claimed) {
            continue;
        }

        idx_slot from = gaps->ranges[i].from;
        idx_slot to = gaps->ranges[i].to;
        idx_slot span = to - from + 1;

        if (span > max_span) {
            /*
             * Narrow the claim and leave the tail claimable, so several
             * fetchers can work one wide hole at once.
             */
            idx_slot claimed_to = from + max_span - 1;
            if (reserve_one(gaps, NULL) != IDX_OK) {
                break; /* cannot split; leave it for the next attempt */
            }
            memmove(&gaps->ranges[i + 1], &gaps->ranges[i],
                    (gaps->count - i) * sizeof(*gaps->ranges));
            gaps->count++;
            gaps->ranges[i].to = claimed_to;
            gaps->ranges[i].claimed = true;
            gaps->ranges[i + 1].from = claimed_to + 1;
            gaps->ranges[i + 1].claimed = false;
            to = claimed_to;
        } else {
            gaps->ranges[i].claimed = true;
        }

        out->from = from;
        out->to = to;
        found = true;
        break;
    }

    refresh_stats(gaps);
    pthread_mutex_unlock(&gaps->lock);
    return found;
}

void idx_gaps_resolve(idx_gaps *gaps, idx_slot from, idx_slot to) {
    if (gaps == NULL || to < from) {
        return;
    }

    pthread_mutex_lock(&gaps->lock);

    for (size_t i = 0; i < gaps->count;) {
        idx_gaps_entry *range = &gaps->ranges[i];

        if (range->to < from || range->from > to) {
            i++;
            continue;
        }

        if (from <= range->from && to >= range->to) {
            gaps->stats.resolved += range->to - range->from + 1;
            remove_at(gaps, i);
            continue;
        }
        if (from <= range->from) {
            gaps->stats.resolved += to - range->from + 1;
            range->from = to + 1;
            i++;
            continue;
        }
        if (to >= range->to) {
            gaps->stats.resolved += range->to - from + 1;
            range->to = from - 1;
            i++;
            continue;
        }

        /*
         * Resolved from the middle, so the range splits. Without room to
         * split, the upper part is kept outstanding rather than dropped:
         * refetching is idempotent, forgetting is not.
         */
        if (reserve_one(gaps, NULL) != IDX_OK) {
            gaps->stats.resolved += to - from + 1;
            range->to = from - 1;
            i++;
            continue;
        }
        memmove(&gaps->ranges[i + 1], &gaps->ranges[i],
                (gaps->count - i) * sizeof(*gaps->ranges));
        gaps->count++;
        gaps->ranges[i].to = from - 1;
        gaps->ranges[i + 1].from = to + 1;
        gaps->stats.resolved += to - from + 1;
        i += 2;
    }

    refresh_stats(gaps);
    pthread_mutex_unlock(&gaps->lock);
}

idx_status idx_gaps_release(idx_gaps *gaps, idx_slot from, idx_slot to,
                            idx_error *err) {
    if (gaps == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null gaps");
    }
    if (to < from) {
        return IDX_OK;
    }

    pthread_mutex_lock(&gaps->lock);
    uint64_t resolved_before = gaps->stats.resolved;
    pthread_mutex_unlock(&gaps->lock);

    /* Drop the claim first: the range may be part of a claimed entry, and
     * re-adding it unclaimed is what makes it available again. */
    idx_gaps_resolve(gaps, from, to);

    pthread_mutex_lock(&gaps->lock);
    /* Restored rather than decremented, because how much of the range was
     * still outstanding is the resolve's business, not ours. Nothing was
     * recovered here: it is going back. */
    gaps->stats.resolved = resolved_before;
    idx_status status = insert(gaps, from, to, err);
    pthread_mutex_unlock(&gaps->lock);
    return status;
}

idx_slot idx_gaps_watermark(const idx_gaps *gaps, idx_slot highest_committed) {
    if (gaps == NULL) {
        return highest_committed;
    }

    idx_gaps *unconst = (idx_gaps *)gaps;
    pthread_mutex_lock(&unconst->lock);
    idx_slot lowest = (gaps->count > 0) ? gaps->ranges[0].from : IDX_SLOT_NONE;
    pthread_mutex_unlock(&unconst->lock);

    if (lowest == IDX_SLOT_NONE) {
        return highest_committed;
    }
    if (lowest == 0) {
        return IDX_SLOT_NONE; /* genesis itself is outstanding */
    }

    idx_slot trustworthy = lowest - 1;
    if (highest_committed == IDX_SLOT_NONE ||
        trustworthy < highest_committed) {
        return trustworthy;
    }
    return highest_committed;
}

bool idx_gaps_is_empty(const idx_gaps *gaps) {
    if (gaps == NULL) {
        return true;
    }
    idx_gaps *unconst = (idx_gaps *)gaps;
    pthread_mutex_lock(&unconst->lock);
    bool empty = (gaps->count == 0);
    pthread_mutex_unlock(&unconst->lock);
    return empty;
}

void idx_gaps_get_stats(const idx_gaps *gaps, idx_gaps_stats *out) {
    if (gaps == NULL || out == NULL) {
        return;
    }
    idx_gaps *unconst = (idx_gaps *)gaps;
    pthread_mutex_lock(&unconst->lock);
    *out = gaps->stats;
    pthread_mutex_unlock(&unconst->lock);
}
