// core/src/knowledge/claim_validator.h
#pragma once

#include <lmdb.h>
#include "types/id.h"
#include "storage/hyper_atom/hyper_atom.h"

typedef enum {
    CLAIM_ACCEPTED = 0,
    CLAIM_ACCEPTED_DUPLICATE,
    CLAIM_REJECTED_CYCLE,           // не сохранено вообще
    CLAIM_FLAGGED_CONTRADICTION,    // сохранено, но CONTRADICTS + штраф
} ClaimVerdict;

// ДОЛЖНА вызываться внутри write-транзакции db_writer, как и hyper_assert*.
ClaimVerdict claim_validate_and_assert(MDB_txn *txn, HyperMemory *hmem,
                                        NeuroAtom *claim, ko_id_t cause_id,
                                        node_id_t ordering_process_id,
                                        uint32_t max_cycle_depth,
                                        node_id_t *out_conflict_id);
