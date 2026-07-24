// knowledge/algorithm_saver.c
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <lmdb.h>

#include "algorithm_saver.h"
#include "types/id.h"
#include "storage/db/db.h"

int algorithm_save(MDB_txn *txn, node_id_t algo_id, const Pipeline *pipeline) {
    if (!pipeline) return -1;

    MDB_val key, data;
    key.mv_size = sizeof(node_id_t);
    key.mv_data = &algo_id;

    /* Вычисляем размер блока */
    size_t total_size = sizeof(uint32_t) * 2; // code_len, capacity
    total_size += pipeline->code_len * sizeof(Instruction);

    /* Добавляем размер констант */
    const ConstantPool *c = &pipeline->constants;
    total_size += sizeof(uint32_t);                     // int_count
    total_size += c->int_count * sizeof(int64_t);
    total_size += sizeof(uint32_t);                     // float_count
    total_size += c->float_count * sizeof(double);
    total_size += sizeof(uint32_t);                     // str_count

    for (uint32_t i = 0; i < c->str_count; i++) {
        total_size += sizeof(uint32_t) + c->str_consts[i].len;
    }

    uint8_t *buffer = malloc(total_size);
    if (!buffer) return -1;

    uint8_t *ptr = buffer;

    /* Заголовки */
    // Безопасная запись uint32_t без нарушения выравнивания
    uint32_t tmp32;

    tmp32 = pipeline->code_len;
    memcpy(ptr, &tmp32, sizeof(tmp32)); ptr += sizeof(tmp32);
    tmp32 = pipeline->capacity;
    memcpy(ptr, &tmp32, sizeof(tmp32)); ptr += sizeof(tmp32);

    /* Байткод */
    if (pipeline->code_len > 0) {
        memcpy(ptr, pipeline->code, pipeline->code_len * sizeof(Instruction));
        ptr += pipeline->code_len * sizeof(Instruction);
    }

    /* Целые константы */
    tmp32 = c->int_count;
    memcpy(ptr, &tmp32, sizeof(tmp32)); ptr += sizeof(tmp32);
    if (c->int_count > 0) {
        memcpy(ptr, c->int_consts, c->int_count * sizeof(int64_t));
        ptr += c->int_count * sizeof(int64_t);
    }

    /* Вещественные константы */
    tmp32 = c->float_count;
    memcpy(ptr, &tmp32, sizeof(tmp32)); ptr += sizeof(tmp32);
    if (c->float_count > 0) {
        memcpy(ptr, c->float_consts, c->float_count * sizeof(double));
        ptr += c->float_count * sizeof(double);
    }

    /* Строковые константы */
    tmp32 = c->str_count;
    memcpy(ptr, &tmp32, sizeof(tmp32)); ptr += sizeof(tmp32);
    for (uint32_t i = 0; i < c->str_count; i++) {
        tmp32 = c->str_consts[i].len;
        memcpy(ptr, &tmp32, sizeof(tmp32)); ptr += sizeof(tmp32);
        if (c->str_consts[i].len > 0) {
            memcpy(ptr, c->str_consts[i].data, c->str_consts[i].len);
            ptr += c->str_consts[i].len;
        }
    }

    data.mv_size = total_size;
    data.mv_data = buffer;

    int rc = mdb_put(txn, db.graph.algorithms, &key, &data, 0);
    free(buffer);
    return rc;
}
