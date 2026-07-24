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

    /* Размер блока: заголовок (2*4) + байткод */
    size_t total_size = sizeof(uint32_t) * 2 + pipeline->code_len * sizeof(Instruction);
    /* Добавляем константы */
    const ConstantPool *c = &pipeline->constants;
    total_size += sizeof(uint32_t);                     // int_count
    total_size += c->int_count * sizeof(int64_t);
    total_size += sizeof(uint32_t);                     // float_count
    total_size += c->float_count * sizeof(double);
    total_size += sizeof(uint32_t);                     // str_count
    for (uint32_t i = 0; i < c->str_count; i++) {
        total_size += sizeof(uint32_t) + c->str_consts[i].len;  // длина + данные
    }

    uint8_t *buffer = malloc(total_size);
    if (!buffer) return -1;

    uint32_t *header = (uint32_t*)buffer;
    header[0] = pipeline->code_len;
    header[1] = pipeline->capacity;
    uint8_t *ptr = buffer + sizeof(uint32_t) * 2;

    /* Байткод */
    memcpy(ptr, pipeline->code, pipeline->code_len * sizeof(Instruction));
    ptr += pipeline->code_len * sizeof(Instruction);

    /* Целые константы */
    *(uint32_t*)ptr = c->int_count;   ptr += sizeof(uint32_t);
    if (c->int_count > 0) {
        memcpy(ptr, c->int_consts, c->int_count * sizeof(int64_t));
        ptr += c->int_count * sizeof(int64_t);
    }

    /* Вещественные константы */
    *(uint32_t*)ptr = c->float_count; ptr += sizeof(uint32_t);
    if (c->float_count > 0) {
        memcpy(ptr, c->float_consts, c->float_count * sizeof(double));
        ptr += c->float_count * sizeof(double);
    }

    /* Строковые константы */
    *(uint32_t*)ptr = c->str_count;   ptr += sizeof(uint32_t);
    for (uint32_t i = 0; i < c->str_count; i++) {
        *(uint32_t*)ptr = c->str_consts[i].len;  ptr += sizeof(uint32_t);
        memcpy(ptr, c->str_consts[i].data, c->str_consts[i].len);
        ptr += c->str_consts[i].len;
    }

    data.mv_size = total_size;
    data.mv_data = buffer;
    int rc = mdb_put(txn, db.graph.algorithms, &key, &data, 0);
    free(buffer);
    return rc;
}
