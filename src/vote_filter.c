#include "vote_filter.h"

bool idx_vote_program_matches(const idx_pubkey *program_id) {
    if (program_id == NULL) {
        return false;
    }
    return idx_pubkey_equal(program_id, &IDX_PROGRAM_VOTE);
}

bool idx_vote_filter_should_drop(const idx_transaction *tx) {
    if (tx == NULL || tx->instruction_count == 0) {
        return false;
    }
    for (size_t i = 0; i < tx->instruction_count; i++) {
        const idx_pubkey *program =
            idx_instruction_program_id(tx, &tx->instructions[i]);
        if (!idx_vote_program_matches(program)) {
            return false;
        }
    }
    return true;
}
