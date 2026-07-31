// knowledge/knowledge_cache.h
#pragma once

#include <lmdb.h>

#include "runtime/vm/vm_context.h"

int knowledge_cache_load_edges(VMContext *ctx, MDB_txn *txn, node_id_t source, node_id_t relation);
int knowledge_cache_load_properties(VMContext *ctx, MDB_txn *txn, node_id_t node_id);
int knowledge_cache_load_embeddings(VMContext *ctx, MDB_txn *txn, node_id_t node_id);
