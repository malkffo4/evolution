#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/instruction.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/edge/edge.h"
#include "storage/graph/graph.h"
#include "storage/string_pool/string_pool.h"
#include "math/hash.h"
#include "knowledge/knowledge_cache.h"

WorkingMemory global_wm;
volatile sig_atomic_t g_running = 1;

int main(void) {
    system("rm -rf ./test_algo_db");
    assert(init_lmdb("./test_algo_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    // 1. Создаём два узла и связь A --CAUSES--> B
    uint64_t A = djb2_hash("A");
    uint64_t B = djb2_hash("B");
    uint64_t REL = djb2_hash("CAUSES");

    Node na = { .id = A, .name_hash = add_string_to_pool(txn, "A"), .type = NODE_CONCEPT };
    Node nb = { .id = B, .name_hash = add_string_to_pool(txn, "B"), .type = NODE_CONCEPT };
    assert(create_node(txn, &na) == MDB_SUCCESS);
    assert(create_node(txn, &nb) == MDB_SUCCESS);

    Edge e = { .key = { A, REL, B }, .confidence = 1.0f, .evidence_count = 1 };
    assert(create_edge(txn, &e) == MDB_SUCCESS);
    mdb_txn_commit(txn);

    // 2. Сохраняем простой алгоритм в LMDB (без LOAD_CONST!)
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    // Алгоритм: одна инструкция CHECK_CACHED_EDGE (проверяет R0,R2,R1 -> R3) и HALT
    Instruction algo_code[] = {
        { .operator_id = OP_CHECK_CACHED_EDGE, .arg[0] = 3, .arg[1] = 0, .arg[2] = 2, .arg[3] = 1 },
        { .operator_id = OP_HALT }
    };

    node_id_t algo_id = djb2_hash("check_edge_algo");
    MDB_val key, data;
    key.mv_size = sizeof(node_id_t);
    key.mv_data = &algo_id;
    data.mv_size = sizeof(algo_code);
    data.mv_data = algo_code;
    assert(mdb_put(txn, db.graph.algorithms, &key, &data, 0) == MDB_SUCCESS);
    mdb_txn_commit(txn);

    // 3. Загружаем кэш и выполняем алгоритм
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == 0);

    VMContext ctx;
    WorkingMemory wm_stub = {0};
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);
    operator_registry_init();

    // Загружаем ребро в кэш VM
    assert(knowledge_cache_load_edges(&ctx, txn, A, REL) == MDB_SUCCESS);

    // Вручную заполняем регистры для алгоритма
    ctx.reg[0].type = REG_INT;  ctx.reg[0].i = (int64_t)A;   // source
    ctx.reg[1].type = REG_INT;  ctx.reg[1].i = (int64_t)B;   // target
    ctx.reg[2].type = REG_INT;  ctx.reg[2].i = (int64_t)REL; // relation

    // Подготавливаем внешний Pipeline: одна инструкция EXEC_ALGORITHM
    ctx.reg[4].type = REG_INT;
    ctx.reg[4].i = (int64_t)algo_id;   // ID алгоритма в регистре 4

    Instruction call = { .operator_id = OP_EXEC_ALGORITHM, .arg[0] = 4 };
    Pipeline outer = { .code = &call, .code_len = 1, .capacity = 1 };
    outer.constants.int_consts = NULL;
    outer.constants.int_count = 0;

    int rc = vm_execute(&ctx, &outer);
    assert(rc == VM_OK);

    // Проверяем результат в R3
    assert(ctx.reg[3].type == REG_BOOL);
    assert(ctx.reg[3].b == true);

    printf("Algorithm test passed: dynamically loaded algorithm executed successfully.\n");

    vm_destroy(&ctx);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_algo_db");
    return 0;
}
