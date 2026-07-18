// tests/knowledge_test.c
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <lmdb.h>

#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/string_pool/string_pool.h"
#include "math/hash.h"
#include "memory/working.h"

// Заглушки глобальных переменных, используемых другими модулями
WorkingMemory global_wm;
volatile sig_atomic_t g_running = 1;

int main(void) {
    // Инициализация БД (создаст каталог ./test_knowledge_db)
    assert(init_lmdb("./test_knowledge_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    // Создаём объект знания (Concept)
    uint64_t id_quicksort = djb2_hash("QuickSort");
    Node obj = {
        .id = id_quicksort,
        .name_hash = add_string_to_pool(txn, "QuickSort"),
        .type = NODE_CONCEPT   // используем существующий тип
    };
    assert(create_node(txn, &obj) == MDB_SUCCESS);

    // Проверяем, что узел существует
    Node retrieved;
    assert(get_node(txn, id_quicksort, &retrieved) == MDB_SUCCESS);
    assert(retrieved.id == id_quicksort);
    assert(retrieved.name_hash == obj.name_hash);
    assert(retrieved.type == NODE_CONCEPT);

    // Эмуляция обновления: удаляем старый и создаём новый с тем же ID
    assert(delete_node(txn, id_quicksort) == MDB_SUCCESS);

    Node updated_obj = {
        .id = id_quicksort,
        .name_hash = add_string_to_pool(txn, "QuickSort (optimized)"),
        .type = NODE_CONCEPT
    };
    assert(create_node(txn, &updated_obj) == MDB_SUCCESS);

    Node updated_retrieved;
    assert(get_node(txn, id_quicksort, &updated_retrieved) == MDB_SUCCESS);
    assert(updated_retrieved.name_hash == updated_obj.name_hash);

    printf("Knowledge lifecycle test passed.\n");

    mdb_txn_commit(txn);
    close_lmdb();

    return 0;
}
