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

    // Проверяем минимальный размер (хотя бы заголовок)
    if (data.mv_size < sizeof(uint32_t) * 2) {
        fprintf(stderr, "Algorithm %lu data too small\n", algo_id);
        return -1;
    }

    const uint8_t *raw = (const uint8_t *)data.mv_data;
    const uint32_t *header = (const uint32_t *)raw;
    uint32_t code_len = header[0];
    // uint32_t capacity = header[1]; // можно игнорировать
    const Instruction *code_start = (const Instruction *)(raw + sizeof(uint32_t) * 2);

    size_t code_bytes = code_len * sizeof(Instruction);
    // Проверяем, что данных достаточно
    if (sizeof(uint32_t) * 2 + code_bytes > data.mv_size) {
        fprintf(stderr, "Algorithm %lu data truncated\n", algo_id);
        return -1;
    }

    Pipeline *p = calloc(1, sizeof(Pipeline));
    if (!p) return -1;

    p->code = malloc(code_bytes);
    if (!p->code) {
        free(p);
        return -1;
    }

    memcpy(p->code, code_start, code_bytes);
    p->code_len = code_len;
    p->capacity = code_len; // можно использовать header[1] при желании
    p->constants.int_consts = NULL;
    p->constants.int_count = 0;

    *out_pipeline = p;
    return 0;
}
