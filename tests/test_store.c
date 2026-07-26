/*
 * Storage abstraction layer tests.
 *
 * The reference backends stand in for the real tiers, so exercising them here
 * pins the contract every backend must satisfy: write persists, reorg deletes
 * at or above a slot and rewrites atomically, prune drops below a slot,
 * read_range gathers a closed slot range for promotion, and the finalized tier
 * appends and flushes. The dispatch wrappers' NULL handling is checked too,
 * since that is the part a caller holding an unconfigured handle depends on.
 */
#include <string.h>

#include "arena.h"
#include "store.h"
#include "test.h"

/* Builds a block-header row for `slot`. */
static idx_store_block_row block_row(idx_slot slot) {
    idx_store_block_row row;
    memset(&row, 0, sizeof(row));
    row.slot = slot;
    row.parent_slot = slot - 1;
    row.has_block_time = true;
    row.block_time = 1700000000 + (int64_t)slot;
    row.transaction_count = 3;
    return row;
}

/* An SOL balance row at `slot`, distinguished by `tag` in the account key. */
static idx_store_sol_balance_row sol_row(idx_slot slot, uint8_t tag) {
    idx_store_sol_balance_row row;
    memset(&row, 0, sizeof(row));
    row.ref.slot = slot;
    row.ref.transaction_index = tag;
    row.balance.account.bytes[0] = tag;
    row.balance.lamports = 1000 + tag;
    row.balance.delta = 5;
    return row;
}

/* A swap row at `slot`, with a bar whose close sits in the same slot. */
static idx_store_swap_row swap_row(idx_slot slot, uint8_t tag) {
    idx_store_swap_row row;
    memset(&row, 0, sizeof(row));
    row.ref.slot = slot;
    row.ref.transaction_index = tag;
    row.swap.venue = IDX_VENUE_PUMP_CURVE;
    row.swap.has_pool = true;
    row.swap.pool.bytes[0] = tag;
    row.has_block_time = true;
    row.block_time = 1700000000 + (int64_t)slot;
    return row;
}

/* A bar whose latest swap (close_seq) is at `slot`, so the store deletes it by
 * that slot on a reorg. */
static idx_bar bar_row(idx_slot slot, uint8_t tag) {
    idx_bar bar;
    memset(&bar, 0, sizeof(bar));
    bar.pool.bytes[0] = tag;
    bar.interval = IDX_BAR_1S;
    bar.bucket = 1700000000 + (int64_t)slot;
    bar.close_seq.slot = slot;
    bar.open_seq.slot = slot;
    bar.close = 1.5;
    return bar;
}

/* A write set carrying one block, one balance, one swap and one bar at `slot`,
 * all borrowing the caller's storage. */
static void one_slot_set(idx_store_write_set *set, const idx_store_block_row *b,
                         const idx_store_sol_balance_row *s,
                         const idx_store_swap_row *sw, const idx_bar *bar) {
    idx_store_write_set_init(set);
    set->blocks = b;
    set->block_count = 1;
    set->sol_balances = s;
    set->sol_balance_count = 1;
    set->swaps = sw;
    set->swap_count = 1;
    set->bars = bar;
    set->bar_count = 1;
}

static void test_write_set_helpers(void) {
    idx_store_write_set set;
    idx_store_write_set_init(&set);
    TEST_EQ_UINT(idx_store_write_set_total(&set), 0);
    TEST_EQ_UINT(idx_store_write_set_total(NULL), 0);

    idx_store_block_row b = block_row(100);
    idx_store_sol_balance_row s = sol_row(100, 1);
    idx_store_swap_row sw = swap_row(100, 1);
    idx_bar bar = bar_row(100, 1);
    one_slot_set(&set, &b, &s, &sw, &bar);
    TEST_EQ_UINT(idx_store_write_set_total(&set), 4);

    idx_store_counts counts = {.blocks = 1, .swaps = 2, .bars = 3};
    TEST_EQ_UINT(idx_store_counts_total(&counts), 6);
    TEST_EQ_UINT(idx_store_counts_total(NULL), 0);
}

static void test_confirmed_write(void) {
    idx_confirmed_store *store = NULL;
    idx_error err;
    idx_error_clear(&err);
    TEST_EQ_INT(idx_mem_confirmed_store_open(&store, &err), IDX_OK);
    TEST_EQ_STR(idx_confirmed_store_name(store), "memory-confirmed");

    idx_store_block_row b = block_row(100);
    idx_store_sol_balance_row s = sol_row(100, 1);
    idx_store_swap_row sw = swap_row(100, 1);
    idx_bar bar = bar_row(100, 1);
    idx_store_write_set set;
    one_slot_set(&set, &b, &s, &sw, &bar);

    TEST_EQ_INT(idx_confirmed_store_write(store, &set, &err), IDX_OK);
    /* A second write accumulates rather than replacing. */
    TEST_EQ_INT(idx_confirmed_store_write(store, &set, &err), IDX_OK);

    idx_store_counts counts;
    idx_mem_confirmed_store_counts(store, &counts);
    TEST_EQ_UINT(counts.blocks, 2);
    TEST_EQ_UINT(counts.sol_balances, 2);
    TEST_EQ_UINT(counts.swaps, 2);
    TEST_EQ_UINT(counts.bars, 2);
    TEST_EQ_UINT(idx_store_counts_total(&counts), 8);

    idx_confirmed_store_close(store);
}

static void test_confirmed_reorg(void) {
    idx_confirmed_store *store = NULL;
    idx_error err;
    idx_error_clear(&err);
    idx_mem_confirmed_store_open(&store, &err);

    /* Three slots in. */
    for (idx_slot slot = 100; slot <= 102; slot++) {
        idx_store_block_row b = block_row(slot);
        idx_store_sol_balance_row s = sol_row(slot, (uint8_t)slot);
        idx_store_swap_row sw = swap_row(slot, (uint8_t)slot);
        idx_bar bar = bar_row(slot, (uint8_t)slot);
        idx_store_write_set set;
        one_slot_set(&set, &b, &s, &sw, &bar);
        TEST_EQ_INT(idx_confirmed_store_write(store, &set, &err), IDX_OK);
    }

    /* Reorg from slot 101: rows at 101 and 102 go, and slot 101 is rewritten
     * (say the corrected block had two swaps instead of one). */
    idx_store_block_row rb = block_row(101);
    idx_store_swap_row rs[2] = {swap_row(101, 201), swap_row(101, 202)};
    idx_bar rbar = bar_row(101, 201);
    idx_store_write_set repl;
    idx_store_write_set_init(&repl);
    repl.blocks = &rb;
    repl.block_count = 1;
    repl.swaps = rs;
    repl.swap_count = 2;
    repl.bars = &rbar;
    repl.bar_count = 1;

    TEST_EQ_INT(idx_confirmed_store_reorg(store, 101, &repl, &err), IDX_OK);

    idx_store_counts counts;
    idx_mem_confirmed_store_counts(store, &counts);
    /* Slot 100 (1 block) survives; slot 101 rewritten (1 block). */
    TEST_EQ_UINT(counts.blocks, 2);
    /* Slot 100's swap survives; slot 101 rewrote two; slot 102's is gone. */
    TEST_EQ_UINT(counts.swaps, 3);
    /* Only slot 100's balance survives — the reorg carried none. */
    TEST_EQ_UINT(counts.sol_balances, 1);
    /* Slot 100's bar survives; slot 101's rewritten; 102's gone. */
    TEST_EQ_UINT(counts.bars, 2);

    /* A reorg with no replacement is a pure delete. */
    TEST_EQ_INT(idx_confirmed_store_reorg(store, 100, NULL, &err), IDX_OK);
    idx_mem_confirmed_store_counts(store, &counts);
    TEST_EQ_UINT(idx_store_counts_total(&counts), 0);

    idx_confirmed_store_close(store);
}

static void test_confirmed_prune(void) {
    idx_confirmed_store *store = NULL;
    idx_error err;
    idx_error_clear(&err);
    idx_mem_confirmed_store_open(&store, &err);

    for (idx_slot slot = 100; slot <= 104; slot++) {
        idx_store_block_row b = block_row(slot);
        idx_store_write_set set;
        idx_store_write_set_init(&set);
        set.blocks = &b;
        set.block_count = 1;
        TEST_EQ_INT(idx_confirmed_store_write(store, &set, &err), IDX_OK);
    }

    /* Retention drops everything below slot 103. */
    TEST_EQ_INT(idx_confirmed_store_prune(store, 103, &err), IDX_OK);
    idx_store_counts counts;
    idx_mem_confirmed_store_counts(store, &counts);
    TEST_EQ_UINT(counts.blocks, 2); /* 103 and 104 remain */

    idx_confirmed_store_close(store);
}

static void test_promotion_read_range(void) {
    idx_confirmed_store *confirmed = NULL;
    idx_finalized_store *finalized = NULL;
    idx_error err;
    idx_error_clear(&err);
    idx_mem_confirmed_store_open(&confirmed, &err);
    idx_mem_finalized_store_open(&finalized, &err);
    TEST_EQ_STR(idx_finalized_store_name(finalized), "memory-finalized");

    for (idx_slot slot = 100; slot <= 103; slot++) {
        idx_store_block_row b = block_row(slot);
        idx_store_swap_row sw = swap_row(slot, (uint8_t)slot);
        idx_store_write_set set;
        idx_store_write_set_init(&set);
        set.blocks = &b;
        set.block_count = 1;
        set.swaps = &sw;
        set.swap_count = 1;
        TEST_EQ_INT(idx_confirmed_store_write(confirmed, &set, &err), IDX_OK);
    }

    idx_arena arena;
    idx_arena_init(&arena, 0);

    /* Promote the closed range [101, 102]. */
    idx_store_write_set promoted;
    TEST_EQ_INT(
        idx_confirmed_store_read_range(confirmed, 101, 102, &arena, &promoted,
                                       &err),
        IDX_OK);
    TEST_EQ_UINT(promoted.block_count, 2);
    TEST_EQ_UINT(promoted.swap_count, 2);
    /* The read gathered exactly the slots asked for. */
    TEST_EQ_UINT(promoted.blocks[0].slot, 101);
    TEST_EQ_UINT(promoted.blocks[1].slot, 102);

    /* Feed the read straight into the finalized tier, no refetch. */
    TEST_EQ_INT(idx_finalized_store_append(finalized, &promoted, &err), IDX_OK);
    TEST_EQ_UINT(idx_mem_finalized_store_pending(finalized), 4);

    idx_store_counts fcounts;
    idx_mem_finalized_store_counts(finalized, &fcounts);
    TEST_EQ_UINT(fcounts.blocks, 2);
    TEST_EQ_UINT(fcounts.swaps, 2);

    TEST_EQ_INT(idx_finalized_store_flush(finalized, &err), IDX_OK);
    TEST_EQ_UINT(idx_mem_finalized_store_pending(finalized), 0);
    TEST_EQ_UINT(idx_mem_finalized_store_flushes(finalized), 1);
    /* Flushing keeps the rows; it only clears the pending count. */
    idx_mem_finalized_store_counts(finalized, &fcounts);
    TEST_EQ_UINT(fcounts.blocks, 2);

    /* An empty range still reads cleanly. */
    idx_store_write_set none;
    TEST_EQ_INT(
        idx_confirmed_store_read_range(confirmed, 200, 300, &arena, &none, &err),
        IDX_OK);
    TEST_EQ_UINT(idx_store_write_set_total(&none), 0);

    idx_arena_destroy(&arena);
    idx_confirmed_store_close(confirmed);
    idx_finalized_store_close(finalized);
}

static void test_finalized_batching(void) {
    idx_finalized_store *store = NULL;
    idx_error err;
    idx_error_clear(&err);
    idx_mem_finalized_store_open(&store, &err);

    idx_store_swap_row sw = swap_row(100, 1);
    idx_store_write_set set;
    idx_store_write_set_init(&set);
    set.swaps = &sw;
    set.swap_count = 1;

    /* Two appends accumulate into one batch, then a flush closes it. */
    TEST_EQ_INT(idx_finalized_store_append(store, &set, &err), IDX_OK);
    TEST_EQ_INT(idx_finalized_store_append(store, &set, &err), IDX_OK);
    TEST_EQ_UINT(idx_mem_finalized_store_pending(store), 2);
    TEST_EQ_INT(idx_finalized_store_flush(store, &err), IDX_OK);
    TEST_EQ_UINT(idx_mem_finalized_store_pending(store), 0);

    /* Appends after a flush start a new batch. */
    TEST_EQ_INT(idx_finalized_store_append(store, &set, &err), IDX_OK);
    TEST_EQ_UINT(idx_mem_finalized_store_pending(store), 1);
    TEST_EQ_UINT(idx_mem_finalized_store_flushes(store), 1);

    idx_finalized_store_close(store);
}

static void test_dispatch_null_handling(void) {
    idx_error err;
    idx_error_clear(&err);

    /* A NULL handle reports rather than crashes. */
    idx_store_write_set set;
    idx_store_write_set_init(&set);
    TEST_EQ_INT(idx_confirmed_store_write(NULL, &set, &err), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_confirmed_store_reorg(NULL, 0, NULL, &err),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_confirmed_store_prune(NULL, 0, &err), IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_finalized_store_append(NULL, &set, &err),
                IDX_ERR_INVALID_ARG);
    TEST_EQ_INT(idx_finalized_store_flush(NULL, &err), IDX_ERR_INVALID_ARG);
    TEST_EQ_STR(idx_confirmed_store_name(NULL), "unset");
    TEST_EQ_STR(idx_finalized_store_name(NULL), "unset");

    /* A backwards range is rejected before it reaches the backend. */
    idx_confirmed_store *store = NULL;
    idx_arena arena;
    idx_arena_init(&arena, 0);
    idx_mem_confirmed_store_open(&store, &err);
    idx_store_write_set out;
    TEST_EQ_INT(
        idx_confirmed_store_read_range(store, 200, 100, &arena, &out, &err),
        IDX_ERR_RANGE);

    /* Introspection on the wrong handle kind is a clean zero, not a misread. */
    idx_store_counts counts;
    idx_mem_finalized_store_counts((idx_finalized_store *)NULL, &counts);
    TEST_EQ_UINT(idx_store_counts_total(&counts), 0);

    idx_arena_destroy(&arena);
    idx_confirmed_store_close(store);
    /* Closing a NULL handle is a no-op. */
    idx_confirmed_store_close(NULL);
    idx_finalized_store_close(NULL);
}

TEST_MAIN({
    TEST_RUN(test_write_set_helpers);
    TEST_RUN(test_confirmed_write);
    TEST_RUN(test_confirmed_reorg);
    TEST_RUN(test_confirmed_prune);
    TEST_RUN(test_promotion_read_range);
    TEST_RUN(test_finalized_batching);
    TEST_RUN(test_dispatch_null_handling);
})
