// knowledge/algorithm_saver.c
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <lmdb.h>

#include "algorithm_saver.h"
#include "types/id.h"
#include "storage/db/db.h"

int algorithm_save(MDB_txn *txn, node_id_t algo_id, const Pipeline *pipeline) {
    MDB_val key, data;
    key.mv_size = sizeof(node_id_t);
    key.mv_data = &algo_id;

    size_t total_size = sizeof(uint32_t) * 2 + pipeline->code_len * sizeof(Instruction);
    uint8_t *buffer = malloc(total_size);
    if (!buffer) return -1;
    uint32_t *header = (uint32_t*)buffer;
    header[0] = pipeline->code_len;
    header[1] = pipeline->capacity;
    memcpy(buffer + sizeof(uint32_t)*2, pipeline->code, pipeline->code_len * sizeof(Instruction));

    data.mv_size = total_size;
    data.mv_data = buffer;
    int rc = mdb_put(txn, db.graph.algorithms, &key, &data, 0);
    free(buffer);
    return rc;
}
