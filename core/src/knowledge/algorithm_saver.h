// knowledge/algorithm_saver.h
#ifndef ALGORITHM_SAVER_H
#define ALGORITHM_SAVER_H

#include <lmdb.h>
#include <stdint.h>

#include "types/id.h"
#include "runtime/compiler/pipeline.h"

int algorithm_save(MDB_txn *txn, node_id_t algo_id, const Pipeline *pipeline);

#endif
