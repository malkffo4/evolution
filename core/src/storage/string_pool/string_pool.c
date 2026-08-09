// storage/string_pool/string_pool.c
#include <stdio.h>
#include <lmdb.h>
#include <stdlib.h>
#include <string.h>

#include "storage/db/db.h"
#include "math/hash.h"
#include "runtime/logging/logging.h"

uint64_t add_string_to_pool(MDB_txn *txn, const char *str) {
    uint64_t hash = djb2_hash(str);
    MDB_val key, data;
    int rc;

    key.mv_size = sizeof(uint64_t);
    key.mv_data = (void *)&hash;

    rc = mdb_get(txn, db.graph.strings, &key, &data);
    if (rc == MDB_SUCCESS) {
        return hash;
    }

    data.mv_size = strlen(str) + 1;
    data.mv_data = (void *)str;

    rc = mdb_put(txn, db.graph.strings, &key, &data, 0);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("mdb_put(strings) failed: %s", mdb_strerror(rc));
        return 0;
    }

    // Сохраняем также маскированные версии хэша!
    // При создании NeuroAtom мы упаковываем типы (ProcKind, HyperType)
    // в старшие биты 64-битного хэша. Из-за этого при retrieve мы ищем строку
    // по маскированному ID и получаем "UNKNOWN". Сохраняя маскированные ключи,
    // мы гарантируем, что retrieve всегда найдет строку.

    // 1. Маска для process_id (срезаются старшие 8 бит)
    uint64_t proc_hash = hash & 0x00FFFFFFFFFFFFFFULL;
    if (proc_hash != hash) {
        key.mv_data = (void *)&proc_hash;
        mdb_put(txn, db.graph.strings, &key, &data, MDB_NOOVERWRITE);
    }

    uint64_t arg_hash = hash & 0x3FFFFFFFFFFFFFFFULL;
    if (arg_hash != hash && arg_hash != proc_hash) {
        key.mv_data = (void *)&arg_hash;
        mdb_put(txn, db.graph.strings, &key, &data, MDB_NOOVERWRITE);
    }

    return hash;
}

const char *get_string_from_pool(MDB_txn *txn, uint64_t hash) {
    // Срезаем биты типа (ProcKind/HyperType), ищем только чистый хэш
    hash = hash & 0x00FFFFFFFFFFFFFFULL;

    MDB_val key, data;
    int rc;

    key.mv_size = sizeof(uint64_t);
    key.mv_data = (void *)&hash;

    rc = mdb_get(txn, db.graph.strings, &key, &data);
    if (rc != MDB_SUCCESS) {
        return NULL;
    }
    return (const char *)data.mv_data;
}
