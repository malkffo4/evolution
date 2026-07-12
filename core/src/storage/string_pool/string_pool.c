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

    /* Проверим, существует ли строка */
    rc = mdb_get(txn, db.graph.strings, &key, &data);
    if (rc == MDB_SUCCESS) {
        LOG_GRAPH("String already exists hash=%016lx value=\"%s\"", hash, str);
        return hash;
    }

    /* Добавим новую строку */
    data.mv_size = strlen(str) + 1;
    data.mv_data = (void *)str;

    rc = mdb_put(txn, db.graph.strings, &key, &data, 0);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("mdb_put(strings) failed: %s", mdb_strerror(rc));
        return 0;
    }

    LOG_GRAPH("Added string hash=%016lx value=\"%s\"", hash, str);

    return hash;
}

// NEEDED FREE
const char *get_string_from_pool(MDB_txn *txn, uint64_t hash) {
    MDB_val key, data;
    int rc;

    key.mv_size = sizeof(uint64_t);
    key.mv_data = (void *)&hash;

    rc = mdb_get(txn, db.graph.strings, &key, &data);
    if (rc != MDB_SUCCESS) {
        LOG_WARN("String not found hash=%016lx", hash);
        return NULL;
    }

    // char *result = (char *)malloc(data.mv_size);
    // if (!result) {
    //     LOG_ERROR( "malloc failed while loading string %016lx", hash);
    //     return NULL;
    // }

    // memcpy(result, data.mv_data, data.mv_size);
    // LOG_DEBUG("Loaded string hash=%016lx value=\"%s\"", hash, result);
    //
    // LMDB — это memory-mapped база данных. `data.mv_data` **уже является прямым указателем на RAM**, где лежит строка!
    // Делая `malloc` и `memcpy`, ты убиваешь кэш процессора и создаешь фрагментацию кучи.
    // * Поскольку строки в пуле неизменяемы (immutable), ты должен возвращать `const char*`, прямо ссылаясь на `data.mv_data`.
    // Это ускорит сравнение смыслов и работу VM в сотни раз и избавит от необходимости бегать по коду с `free()`.

    return (const char *)data.mv_data;
}
