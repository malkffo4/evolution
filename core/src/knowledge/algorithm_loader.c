// knowledge/algorithm_loader.c
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "algorithm_loader.h"
#include "storage/db/db.h"

int algorithm_load(MDB_txn *txn, node_id_t algo_id, Pipeline **out_pipeline) {
    if (!out_pipeline) return -1;

    MDB_val key, data;
    key.mv_size = sizeof(node_id_t);
    key.mv_data = &algo_id;

    int rc = mdb_get(txn, db.graph.algorithms, &key, &data);
    if (rc != MDB_SUCCESS) {
        fprintf(stderr, "Algorithm %lu not found in DB\n", algo_id);
        return rc;
    }

    size_t num_instructions = data.mv_size / sizeof(Instruction);

    Pipeline *p = calloc(1, sizeof(Pipeline));
    if (!p) return -1;

    p->code = malloc(data.mv_size);
    if (!p->code) {
        free(p);
        return -1;
    }

    memcpy(p->code, data.mv_data, data.mv_size);
    p->code_len = num_instructions;
    p->capacity = num_instructions;
    // Константы пока не сериализуем, OP_LOAD_CONST берёт значение из arg[1]
    p->constants.int_consts = NULL;
    p->constants.float_consts = NULL;
    p->constants.str_consts = NULL;
    p->constants.int_count = 0;
    p->constants.float_count = 0;
    p->constants.str_count = 0;

    *out_pipeline = p;
    return 0;
}
