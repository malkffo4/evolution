#include <stdio.h>
#include <stdlib.h>
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
#include "knowledge/knowledge_cache.h"   // наш новый модуль

WorkingMemory global_wm;
volatile sig_atomic_t g_running = 1;

int main(void) {
    // 1. Инициализация временной БД
    system("rm -rf ./test_cog_db");
    assert(init_lmdb("./test_cog_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    // 2. Создаём два узла и ребро CAUSES
    uint64_t node_a = djb2_hash("A");
    uint64_t node_b = djb2_hash("B");
    uint64_t rel_causes = djb2_hash("CAUSES");

    Node na = { .id = node_a, .name_hash = add_string_to_pool(txn, "A"), .type = NODE_CONCEPT };
    Node nb = { .id = node_b, .name_hash = add_string_to_pool(txn, "B"), .type = NODE_CONCEPT };
    assert(create_node(txn, &na) == MDB_SUCCESS);
    assert(create_node(txn, &nb) == MDB_SUCCESS);

    Edge edge = {
        .key = { .source = node_a, .target = node_b, .relation = rel_causes },
        .confidence = 1.0f,
        .evidence_count = 1
    };
    assert(create_edge(txn, &edge) == MDB_SUCCESS);
    mdb_txn_commit(txn);

    // 3. Новая транзакция для чтения и кеширования
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == 0);

    VMContext ctx;
    // инициализируем VM (включая трассировку, арену и т.д.)
    WorkingMemory wm_stub = {0};
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);
    operator_registry_init();

    // Загружаем в кеш все рёбра с отношением CAUSES от узла A
    assert(knowledge_cache_load_edges(&ctx, txn, node_a, rel_causes) == MDB_SUCCESS);

    // 4. Строим Pipeline: проверяем, есть ли ребро A --CAUSES--> B
    Pipeline pipeline = {0};
    // Загружаем ID узлов и отношение как константы
    pipeline.constants.int_consts = malloc(3 * sizeof(int64_t));
    pipeline.constants.int_consts[0] = (int64_t)node_a;
    pipeline.constants.int_consts[1] = (int64_t)node_b;
    pipeline.constants.int_consts[2] = (int64_t)rel_causes;
    pipeline.constants.int_count = 3;

    // LOAD A в R0
    Instruction load_a = { .operator_id = OP_LOAD_CONST, .arg[0] = 0, .arg[1] = 0 };
    // LOAD B в R1
    Instruction load_b = { .operator_id = OP_LOAD_CONST, .arg[0] = 1, .arg[1] = 1 };
    // LOAD CAUSES в R2
    Instruction load_rel = { .operator_id = OP_LOAD_CONST, .arg[0] = 2, .arg[1] = 2 };
    // CHECK_CACHED_EDGE R3 = (R0, R2, R1)
    Instruction check = {
        .operator_id = OP_CHECK_CACHED_EDGE,
        .arg[0] = 3, .arg[1] = 0, .arg[2] = 2, .arg[3] = 1
    };
    Instruction halt = { .operator_id = OP_HALT };

    Instruction prog[] = { load_a, load_b, load_rel, check, halt };
    pipeline.code = prog;
    pipeline.code_len = sizeof(prog) / sizeof(Instruction);

    int rc = vm_execute(&ctx, &pipeline);
    assert(rc == VM_OK);

    printf("R3 type=%d, bool=%d\n", ctx.reg[3].type, (int)ctx.reg[3].b);
    printf("preloaded_edge_count=%u\n", ctx.preloaded_edge_count);
    // Результат в R3 должен быть true
    assert(ctx.reg[3].type == REG_BOOL);
    assert(ctx.reg[3].b == true);

    printf("Cognitive test passed: A --CAUSES--> B is true.\n");

    // Очистка
    free(pipeline.constants.int_consts);
    vm_destroy(&ctx);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_cog_db");
    return 0;
}
