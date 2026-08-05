// reasoning/algorithm_planner.h
#pragma once

#include <lmdb.h>

#include "types/id.h"
#include "runtime/vm/vm_context.h"

#define MAX_CANDIDATES_ALGO     16

size_t find_goal_algorithm_relations(MDB_txn *txn, HyperMemory *hmem, node_id_t *rel_ids, size_t max_rels);
void invalidate_goal_algorithm_relation_cache(void);
