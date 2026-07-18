#include <stdio.h>
#include <string.h>
#include <stdlib.h>    // для system()
#include <assert.h>
#include <signal.h>
#include <lmdb.h>

#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/edge/edge.h"
#include "storage/string_pool/string_pool.h"
#include "math/hash.h"
#include "memory/working.h"

// Заглушки глобальных переменных, используемых другими модулями
WorkingMemory global_wm;
volatile sig_atomic_t g_running = 1;

int main(void) {
    // Удаляем старую тестовую базу, если она есть
    system("rm -rf ./test_mem_db");

    // Инициализация БД
    assert(init_lmdb("./test_mem_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    // Создаём два узла-концепта
    uint64_t id_stack = djb2_hash("Stack");
    uint64_t id_ds = djb2_hash("DataStructure");

    Node stack_node = {
        .id = id_stack,
        .name_hash = add_string_to_pool(txn, "Stack"),
        .type = NODE_CONCEPT
    };
    Node ds_node = {
        .id = id_ds,
        .name_hash = add_string_to_pool(txn, "DataStructure"),
        .type = NODE_CONCEPT
    };

    assert(create_node(txn, &stack_node) == MDB_SUCCESS);
    assert(create_node(txn, &ds_node) == MDB_SUCCESS);

    // Создаём отношение is_a
    uint64_t rel_is_a = add_string_to_pool(txn, "is_a");
    Edge edge = {
        .key = { .source = id_stack, .target = id_ds, .relation = rel_is_a },
        .confidence = 1.0f,
        .evidence_count = 1
    };
    // На чистой базе create_edge гарантированно выполнится без MDB_KEYEXIST
    assert(create_edge(txn, &edge) == MDB_SUCCESS);

    // Проверяем узел
    Node retrieved;
    assert(get_node(txn, id_stack, &retrieved) == MDB_SUCCESS);
    assert(retrieved.name_hash == stack_node.name_hash);

    // Проверяем ребро
    Edge edge_ret;
    Triple key = { id_stack, rel_is_a, id_ds };
    int rc = get_edge(txn, &key, &edge_ret);
    assert(rc == MDB_SUCCESS);
    assert(edge_ret.confidence == 1.0f);

    mdb_txn_commit(txn);
    close_lmdb();

    printf("Memory test passed.\n");
    return 0;
}
