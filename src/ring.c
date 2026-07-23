#include "ring.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

/* The queue stores the caller's entry verbatim; only seq is ours. */
typedef idx_ring_entry idx_ring_desc;

struct idx_ring {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;

    idx_ring_desc *queue;
    size_t depth;
    uint64_t head; /* next publish position */
    uint64_t tail; /* next consume position */

    uint64_t next_seq;
    bool closed;

    idx_ring_stats stats;
};

void idx_ring_options_init(idx_ring_options *options) {
    if (options == NULL) {
        return;
    }
    options->depth = IDX_RING_DEFAULT_DEPTH;
}

/* Caller holds the lock. */
static void note_queued(idx_ring *ring) {
    size_t queued = (size_t)(ring->head - ring->tail);
    ring->stats.queued = queued;
    if (queued > ring->stats.high_water) {
        ring->stats.high_water = queued;
    }
}

idx_status idx_ring_new(const idx_ring_options *options, idx_ring **out,
                        idx_error *err) {
    if (out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null output parameter");
    }

    size_t depth = IDX_RING_DEFAULT_DEPTH;
    if (options != NULL && options->depth != 0) {
        depth = options->depth;
    }

    /* A depth of one leaves the producer nothing to drop but the entry the
     * consumer may be holding. */
    if (depth < 2) {
        return IDX_FAIL(err, IDX_ERR_RANGE, "ring depth %zu must be at least 2",
                        depth);
    }

    idx_ring *ring = calloc(1, sizeof(*ring));
    if (ring == NULL) {
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "out of memory");
    }
    ring->queue = calloc(depth, sizeof(*ring->queue));
    if (ring->queue == NULL) {
        free(ring);
        return IDX_FAIL(err, IDX_ERR_NO_MEMORY, "out of memory");
    }
    ring->depth = depth;
    ring->stats.depth = depth;

    pthread_mutex_init(&ring->lock, NULL);

    /* A monotonic condvar clock, so a wall-clock adjustment cannot turn a
     * timed wait into a long sleep. */
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&ring->not_empty, &attr);
    pthread_condattr_destroy(&attr);

    *out = ring;
    return IDX_OK;
}

void idx_ring_free(idx_ring *ring) {
    if (ring == NULL) {
        return;
    }
    while (ring->tail != ring->head) {
        idx_json_free(ring->queue[ring->tail % ring->depth].doc);
        ring->tail++;
    }
    pthread_cond_destroy(&ring->not_empty);
    pthread_mutex_destroy(&ring->lock);
    free(ring->queue);
    free(ring);
}

/* Caller holds the lock. Frees the oldest queued document. */
static void drop_oldest(idx_ring *ring) {
    idx_ring_desc *oldest = &ring->queue[ring->tail % ring->depth];

    IDX_DEBUG("ring full: dropped slot %llu (seq %llu)",
              (unsigned long long)oldest->slot,
              (unsigned long long)oldest->seq);

    idx_json_free(oldest->doc);
    oldest->doc = NULL;
    ring->tail++;
    ring->stats.dropped++;
}

idx_status idx_ring_publish(idx_ring *ring, const idx_ring_entry *entry,
                            idx_error *err) {
    if (ring == NULL || entry == NULL) {
        if (entry != NULL) {
            idx_json_free(entry->doc);
        }
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null argument");
    }

    pthread_mutex_lock(&ring->lock);

    if (ring->closed) {
        pthread_mutex_unlock(&ring->lock);
        /* Freed rather than handed back: the caller has no way to publish it
         * anywhere else, and a returned pointer here is a leak waiting to
         * happen on the shutdown path. */
        idx_json_free(entry->doc);
        return IDX_FAIL(err, IDX_ERR_CLOSED, "the ring is closed");
    }

    if ((size_t)(ring->head - ring->tail) >= ring->depth) {
        drop_oldest(ring);
    }

    idx_ring_desc *desc = &ring->queue[ring->head % ring->depth];
    *desc = *entry;
    desc->seq = ring->next_seq++;
    ring->head++;

    ring->stats.published++;
    ring->stats.bytes += entry->bytes;
    note_queued(ring);

    pthread_cond_signal(&ring->not_empty);
    pthread_mutex_unlock(&ring->lock);
    return IDX_OK;
}

/* Absolute monotonic deadline `timeout_ms` from now. */
static void deadline_from_now(struct timespec *out, int timeout_ms) {
    clock_gettime(CLOCK_MONOTONIC, out);
    out->tv_sec += timeout_ms / 1000;
    out->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec += 1;
        out->tv_nsec -= 1000000000L;
    }
}

idx_status idx_ring_consume(idx_ring *ring, int timeout_ms, idx_ring_entry *out,
                            idx_error *err) {
    if (ring == NULL || out == NULL) {
        return IDX_FAIL(err, IDX_ERR_INVALID_ARG, "null argument");
    }

    struct timespec deadline;
    deadline_from_now(&deadline, (timeout_ms > 0) ? timeout_ms : 0);

    pthread_mutex_lock(&ring->lock);

    while (ring->head == ring->tail) {
        /* Queued entries first, then the close: a producer shutting down never
         * costs the consumer what it already published. */
        if (ring->closed) {
            pthread_mutex_unlock(&ring->lock);
            return IDX_ERR_CLOSED;
        }
        if (timeout_ms <= 0) {
            pthread_mutex_unlock(&ring->lock);
            return IDX_ERR_TIMEOUT;
        }
        int waited =
            pthread_cond_timedwait(&ring->not_empty, &ring->lock, &deadline);
        if (waited == ETIMEDOUT) {
            pthread_mutex_unlock(&ring->lock);
            return IDX_ERR_TIMEOUT;
        }
    }

    idx_ring_desc *desc = &ring->queue[ring->tail % ring->depth];
    *out = *desc;
    desc->doc = NULL; /* ownership moves to the caller */
    ring->tail++;

    ring->stats.consumed++;
    note_queued(ring);

    pthread_mutex_unlock(&ring->lock);
    return IDX_OK;
}

void idx_ring_release(idx_ring *ring, idx_ring_entry *entry) {
    (void)ring;
    if (entry == NULL) {
        return;
    }
    idx_json_free(entry->doc);
    entry->doc = NULL;
}

void idx_ring_close(idx_ring *ring) {
    if (ring == NULL) {
        return;
    }
    pthread_mutex_lock(&ring->lock);
    ring->closed = true;
    pthread_cond_broadcast(&ring->not_empty);
    pthread_mutex_unlock(&ring->lock);
}

void idx_ring_get_stats(const idx_ring *ring, idx_ring_stats *out) {
    if (ring == NULL || out == NULL) {
        return;
    }
    idx_ring *unconst = (idx_ring *)ring;
    pthread_mutex_lock(&unconst->lock);
    *out = ring->stats;
    pthread_mutex_unlock(&unconst->lock);
}
