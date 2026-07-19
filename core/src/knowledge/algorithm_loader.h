// knowledge/algorithm_loader.h
#ifndef ALGORITHM_LOADER_H
#define ALGORITHM_LOADER_H

#include <lmdb.h>
#include "types/id.h"
#include "runtime/compiler/pipeline.h"

int algorithm_load(MDB_txn *txn, node_id_t algo_id, Pipeline **out_pipeline);

#endif  // ALGORITHM_LOADER_H
