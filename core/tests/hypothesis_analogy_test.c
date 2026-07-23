// tests/hypothesis_analogy_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <math.h>
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
#include "storage/vector_store/vector_store.h"
#include "math/hash.h"
#include "knowledge/knowledge_cache.h"

WorkingMemory global_wm;
volatile sig_atomic_t g_running = 1;

// Подготовка эмбеддингов для теста (в реальности их даст LLM/модель)
static void prepare_embeddings(MDB_txn *txn) {
    float emb_b[EMBEDDING_DIM] = {1.0f, 0.1f, 0.0f, 0.0f, 0.0f}; // остальные нули
    float emb_c[EMBEDDING_DIM] = {0.9f, 0.2f, 0.0f, 0.0f, 0.0f};
    // Сохраним через save_embedding
    save_embedding(txn, djb2_hash("B"), emb_b);
    save_embedding(txn, djb2_hash("C"), emb_c);
}

int main(void) {
    system("rm -rf ./test_hyp_db");
    assert(init_lmdb("./test_hyp_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    // 1. Создаём узлы и рёбра
    uint64_t A = djb2_hash("A");
    uint64_t B = djb2_hash("B");
    uint64_t C = djb2_hash("C");
    uint64_t D = djb2_hash("D");
    uint64_t REL = djb2_hash("CAUSES");

    Node na = { .id = A, .name_hash = add_string_to_pool(txn, "A"), .type = NODE_CONCEPT };
    Node nb = { .id = B, .name_hash = add_string_to_pool(txn, "B"), .type = NODE_CONCEPT };
    Node nc = { .id = C, .name_hash = add_string_to_pool(txn, "C"), .type = NODE_CONCEPT };
    Node nd = { .id = D, .name_hash = add_string_to_pool(txn, "D"), .type = NODE_CONCEPT };
    assert(create_node(txn, &na) == MDB_SUCCESS);
    assert(create_node(txn, &nb) == MDB_SUCCESS);
    assert(create_node(txn, &nc) == MDB_SUCCESS);
    assert(create_node(txn, &nd) == MDB_SUCCESS);

    Edge e1 = { .key = { A, REL, B }, .confidence = 1.0f, .evidence_count = 1 };
    Edge e2 = { .key = { C, REL, D }, .confidence = 1.0f, .evidence_count = 1 };
    assert(create_edge(txn, &e1) == MDB_SUCCESS);
    assert(create_edge(txn, &e2) == MDB_SUCCESS);

    // Подготовим эмбеддинги для B и C (чтобы работал find_similar)
    prepare_embeddings(txn);
    mdb_txn_commit(txn);

    // 2. Фаза Virtual Mind: предзагрузка
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == 0);

    VMContext ctx;
    WorkingMemory wm_stub = {0};
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);
    operator_registry_init();

    // Загружаем рёбра в кеш
    assert(knowledge_cache_load_edges(&ctx, txn, A, REL) == MDB_SUCCESS);
    assert(knowledge_cache_load_edges(&ctx, txn, C, REL) == MDB_SUCCESS);

    // Загружаем эмбеддинги в scratchpad
    assert(knowledge_cache_load_embeddings(&ctx, txn, djb2_hash("B")) == MDB_SUCCESS);
    assert(knowledge_cache_load_embeddings(&ctx, txn, djb2_hash("C")) == MDB_SUCCESS);

    // 3. Настройка регистров (теперь используются реальные хеши)
    ctx.reg[1].type = REG_INT; ctx.reg[1].i = (int64_t)A;   // start
    ctx.reg[2].type = REG_INT; ctx.reg[2].i = (int64_t)D;   // end
    ctx.reg[3].type = REG_INT; ctx.reg[3].i = (int64_t)REL; // relation

    // 4. Pipeline гипотезы (теперь find_similar без sp_start/count)
    Instruction code[] = {
        // [0] Получить соседей A → scratchpad[0..], кол-во в R7
        { .operator_id = OP_GET_NEIGHBORS, .arg = {1, 3, 0, 7} },
        // [1] Прочитать первого соседа (B) в R4
        { .operator_id = OP_READ_SP, .arg = {4, 0} },
        // [2] Найти похожий на B (целевой регистр R4, результат в scratchpad[20])
        { .operator_id = OP_FIND_SIMILAR, .arg = {4, 0, 0, 20} },  // sp_start/count теперь не важны
        // [3] Прочитать аналог (C) в R5
        { .operator_id = OP_READ_SP, .arg = {5, 20} },
        // [4] Получить соседей C → scratchpad[30..]
        { .operator_id = OP_GET_NEIGHBORS, .arg = {5, 3, 30, 7} },
        // [5] Прочитать соседа C (D) в R6
        { .operator_id = OP_READ_SP, .arg = {6, 30} },
        // [6] Склеить путь → scratchpad[50]
        { .operator_id = OP_CONCAT_PATHS, .arg = {50, 1, 4, 5, 6} },
        // [7] HALT
        { .operator_id = OP_HALT }
    };

    Pipeline pipeline = { .code = code, .code_len = 8, .capacity = 8 };
    pipeline.constants.int_consts = NULL;
    pipeline.constants.int_count = 0;

    printf("preloaded_edge_count = %u\n", ctx.preloaded_edge_count);

    int rc = vm_execute(&ctx, &pipeline);
    assert(rc == VM_OK);

    printf("candidate after find_similar: %lu\n", (unsigned long)ctx.scratchpad[20].key_hash);

    // 5. Проверка результата
    char *result = (char*)(uintptr_t)ctx.scratchpad[50].value;
    assert(result != NULL);
    printf("Hypothesis path: %s\n", result);

    // Проверяем, что путь содержит все четыре узла (в любом порядке, главное структура)
    char expected[256];
    snprintf(expected, sizeof(expected), "Path(%lu -> %lu ~> %lu -> %lu)",
             (unsigned long)A, (unsigned long)B, (unsigned long)C, (unsigned long)D);
    assert(strcmp(result, expected) == 0);
    printf("Hypothesis analogy test passed.\n");
    free(result);
    // Освобождаем эмбеддинги, лежащие в слотах 32..47
    for (int i = 32; i < 48; i++) {
        if (ctx.scratchpad[i].value && (uintptr_t)ctx.scratchpad[i].value > 0x1000) {
            free((float*)(uintptr_t)ctx.scratchpad[i].value);
        }
    }
    vm_destroy(&ctx);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_hyp_db");
    printf("Hypothesis analogy test 2 passed.\n");
    return 0;
}
