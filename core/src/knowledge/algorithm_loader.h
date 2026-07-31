// knowledge/algorithm_loader.h
#pragma once

#include <lmdb.h>
#include "types/id.h"
#include "runtime/compiler/pipeline.h"

int algorithm_load(MDB_txn *txn, node_id_t algo_id, Pipeline **out_pipeline);
