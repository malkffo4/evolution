// knowledge/hyper_retrieval.h
#pragma once

#include <lmdb.h>

#include "types/id.h"
#include "storage/hyper_atom/hyper_atom.h"

char* hyper_retrieve_json(MDB_txn *txn, HyperMemory *hmem, node_id_t participant_id, int max_depth, int max_atoms);
