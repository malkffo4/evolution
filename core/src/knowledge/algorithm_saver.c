// knowledge/algorithm_saver.c
#include <string.h>
#include <stdlib.h>

#include "algorithm_saver.h"
#include "storage/db/db.h"

int algorithm_save(MDB_txn *txn, node_id_t algo_id, const Instruction *code, uint32_t code_len) {
    if (!txn || !code || code_len == 0) return -1;

    MDB_val key, data;
    key.mv_size = sizeof(node_id_t);
    key.mv_data = &algo_id;
    data.mv_size = code_len * sizeof(Instruction);
    data.mv_data = (void *)code;

    return mdb_put(txn, db.graph.algorithms, &key, &data, 0);
}
