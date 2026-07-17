#ifndef RETRIEVAL_H
#define RETRIEVAL_H

#include <lmdb.h>
#include "types/id.h"

// Извлекает граф из БД в формате JSON для передачи в Python RAG
char* retrieve_subgraph_json(MDB_txn *txn, node_id_t start_node, int max_depth, int max_nodes);

#endif // RETRIEVAL_H
