// reasoning/algorithm_planner.h
#ifndef ALGORITHM_PLANNER_H
#define ALGORITHM_PLANNER_H

#include <lmdb.h>

#include "types/id.h"
#include "runtime/vm/vm_context.h"

#define MAX_CANDIDATES_ALGO     16

int planner_select_algorithm(HyperMemory *txn, node_id_t goal_id, VMContext *ctx, node_id_t *out_algo_id);

#endif
