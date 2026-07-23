/*
 * Bounded hand-off between the receive thread and the processing thread
 * (decision D6).
 *
 * The socket is never allowed to wait. A producer that finds the ring full
 * drops its oldest entry, so publishing always completes in bounded time; the
 * consumer sees the resulting jump in sequence numbers and reports the slots
 * it lost to the gap fetcher. Losing a slot here costs a getBlock over the
 * recovery path, which is why this ring is deliberately unreliable rather than
 * backpressuring the connection.
 *
 * What travels is the parsed document, by pointer and by ownership. The
 * PubSub layer already parses each notification into a self-contained
 * idx_json_doc in order to demultiplex it, so handing that over costs nothing:
 * no copy of a multi-megabyte payload, and no second parse on the far side.
 * The ring owns every document it holds and frees the ones it drops.
 *
 * One producer and one consumer. Both may run concurrently; neither may be
 * used from more than one thread.
 */
#ifndef IDX_RING_H
#define IDX_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "json.h"
#include "types.h"

typedef struct idx_ring idx_ring;

#define IDX_RING_DEFAULT_DEPTH 8u

typedef struct {
    /*
     * Documents that may be queued at once. A parsed block runs to tens of
     * megabytes, so this is a memory setting as much as a latency one: deeper
     * rides out a longer stall downstream, shallower drops sooner, and a drop
     * costs a refetch rather than data. 0 selects IDX_RING_DEFAULT_DEPTH.
     */
    size_t depth;
} idx_ring_options;

void idx_ring_options_init(idx_ring_options *options);

/*
 * One entry, filled in by the producer and handed to the consumer. The same
 * struct describes both directions; `seq` is the ring's to assign and is
 * ignored on the way in.
 */
typedef struct {
    /*
     * Publication sequence, counting from 0 and never reused. A consumer that
     * sees this advance by more than one knows the difference was dropped, and
     * knows it without having to consult the producer.
     */
    uint64_t seq;

    idx_slot slot;

    /* Ownership passes to the ring on publish and back out on consume. */
    idx_json_doc *doc;

    /*
     * A node inside `doc` that the producer already located — the point of
     * carrying it is that the consumer does not navigate, or parse, again.
     * Borrowed: valid exactly as long as `doc` is.
     */
    idx_json_val value;

    /* Application-defined. The pipeline records which transport delivered
     * the block. */
    uint64_t tag;

    /* Size of the payload this came from, kept because the bytes themselves
     * are gone by now and the figure is still worth reporting. */
    size_t bytes;
} idx_ring_entry;

typedef struct {
    uint64_t published;
    uint64_t consumed;
    uint64_t dropped; /* freed before anyone consumed them */
    uint64_t bytes;
    size_t depth;      /* what the ring was sized for */
    size_t queued;     /* entries waiting right now */
    size_t high_water; /* deepest the queue has been */
} idx_ring_stats;

idx_status idx_ring_new(const idx_ring_options *options, idx_ring **out,
                        idx_error *err);

/* Frees the ring and every document still queued in it. */
void idx_ring_free(idx_ring *ring);

/*
 * Queues `entry`, taking ownership of its document. Never blocks on the
 * consumer: a full ring frees its oldest entry instead, which is counted in
 * `dropped` and shows up as a sequence gap on the other side.
 *
 *   IDX_OK         queued; the ring owns the document from here
 *   IDX_ERR_CLOSED the ring is closed. The document is freed rather than
 *                  leaked, so the caller must not touch it either way
 */
idx_status idx_ring_publish(idx_ring *ring, const idx_ring_entry *entry,
                            idx_error *err);

/*
 * Takes the oldest queued entry, waiting up to `timeout_ms` for one. A
 * timeout of 0 polls without blocking.
 *
 *   IDX_OK          `out` holds an entry the caller must release
 *   IDX_ERR_TIMEOUT nothing arrived in the budget
 *   IDX_ERR_CLOSED  the ring is closed and drained; no further entry will come
 *
 * Every entry obtained here must be released, including on the paths that stop
 * the pipeline.
 */
idx_status idx_ring_consume(idx_ring *ring, int timeout_ms, idx_ring_entry *out,
                            idx_error *err);

/* Frees the entry's document. */
void idx_ring_release(idx_ring *ring, idx_ring_entry *entry);

/*
 * Refuses further publishing and wakes a waiting consumer. Entries already
 * queued are still delivered, so a consumer draining until IDX_ERR_CLOSED sees
 * everything the producer managed to publish. Idempotent.
 */
void idx_ring_close(idx_ring *ring);

void idx_ring_get_stats(const idx_ring *ring, idx_ring_stats *out);

#endif /* IDX_RING_H */
