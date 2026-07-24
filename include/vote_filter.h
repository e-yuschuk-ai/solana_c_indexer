/*
 * Vote transaction filter (ROADMAP.md milestone M6).
 *
 * Every validator votes every slot, so votes are the majority of the
 * transactions in a mainnet block, and decision D5 keeps nothing they produce:
 * no transfer, no swap, no balance this indexer holds. They are dropped before
 * any extraction runs, which is the single largest lever on storage volume in
 * the project and costs one program-id comparison per instruction.
 *
 * The test is deliberately conservative. Failing to recognise a vote costs
 * storage; mistaking something else for one loses an event that cannot be
 * recovered without refetching the block, so a transaction is a vote only when
 * every instruction in it is one.
 */
#ifndef IDX_VOTE_FILTER_H
#define IDX_VOTE_FILTER_H

#include <stdbool.h>

#include "block.h"
#include "types.h"

/* True for the Vote program, which is the only program a vote invokes. */
bool idx_vote_program_matches(const idx_pubkey *program_id);

/*
 * True when `tx` is a vote and extraction should skip it: it has at least one
 * instruction and every top-level one invokes the Vote program.
 *
 * Inner instructions are not examined. The Vote program calls nothing, so a
 * transaction whose top-level instructions are all votes has no inner ones —
 * and a transaction that does invoke something else fails the test on that
 * instruction before its inner ones matter.
 *
 * A transaction with no instructions is not a vote. It carries nothing either,
 * but saying otherwise would make the empty case the one shape that reaches
 * the extractors as a vote.
 */
bool idx_vote_filter_should_drop(const idx_transaction *tx);

#endif /* IDX_VOTE_FILTER_H */
