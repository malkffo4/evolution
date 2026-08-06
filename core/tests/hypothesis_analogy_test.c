// tests/hypothesis_analogy_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/string_pool/string_pool.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"
#include "knowledge/knowledge_cache.h"

static void prepare_embeddings(MDB_txn *txn, MDB_dbi dbi_vectors) {
    Vector128 emb_b = { .data = {1.0f, 0.1f} };   // остальные 126 нулей
    Vector128 emb_c = { .data = {0.9f, 0.2f} };
    hyper_vector_save(txn, dbi_vectors, djb2_hash("B"), &emb_b);
    hyper_vector_save(txn, dbi_vectors, djb2_hash("C"), &emb_c);
}

int main(void) {
    system("rm -rf ./test_hyp_db");
    assert(init_lmdb("./test_hyp_db") == MDB_SUCCESS);// ── открываем векторный индекс ──

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);
    MDB_dbi dbi_vectors;
    assert(mdb_dbi_open(txn, "hyper_idx_vectors", MDB_CREATE, &dbi_vectors) == MDB_SUCCESS);
    mdb_txn_commit(txn);

    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    uint64_t A = djb2_hash("A");
    uint64_t B = djb2_hash("B");
    uint64_t C = djb2_hash("C");
    uint64_t D = djb2_hash("D");
    uint64_t REL = djb2_hash("CAUSES");

    // Узлы и рёбра (старый граф, нужно для предзагрузки)
    Node na = { .id = A, .name_hash = add_string_to_pool(txn, "A"), .type = NODE_CONCEPT };
    Node nb = { .id = B, .name_hash = add_string_to_pool(txn, "B"), .type = NODE_CONCEPT };
    Node nc = { .id = C, .name_hash = add_string_to_pool(txn, "C"), .type = NODE_CONCEPT };
    Node nd = { .id = D, .name_hash = add_string_to_pool(txn, "D"), .type = NODE_CONCEPT };
    create_node(txn, &na); create_node(txn, &nb);
    create_node(txn, &nc); create_node(txn, &nd);

    Edge e1 = { .key = { A, REL, B }, .confidence = 1.0f, .evidence_count = 1 };
    Edge e2 = { .key = { C, REL, D }, .confidence = 1.0f, .evidence_count = 1 };
    create_edge(txn, &e1);
    create_edge(txn, &e2);

    prepare_embeddings(txn, dbi_vectors);
    mdb_txn_commit(txn);

    // ── исполнение ──
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == 0);

    VMContext ctx;
    WorkingMemory wm_stub = {0};
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);
    operator_registry_init();

    // Инициализируем HyperMemory для find_similar (нужен доступ к dbi_idx_vectors)
    ctx.hyper_mem = hyper_memory_new(db.graph.hyper.atoms,
        db.graph.hyper.idx_process,
        db.graph.hyper.idx_args,
        db.graph.hyper.idx_context);
    assert(ctx.hyper_mem != NULL);
    ctx.hyper_mem->dbi_idx_vectors = dbi_vectors;   // прокидываем вручную

    // Загружаем рёбра
    knowledge_cache_load_edges(&ctx, txn, A, REL);
    knowledge_cache_load_edges(&ctx, txn, C, REL);

    // Регистры
    ctx.reg[1].type = REG_INT; ctx.reg[1].i = (int64_t)A;   // start
    ctx.reg[2].type = REG_INT; ctx.reg[2].i = (int64_t)D;   // end
    ctx.reg[3].type = REG_INT; ctx.reg[3].i = (int64_t)REL; // relation
    ctx.reg[8].type = REG_FLOAT; ctx.reg[8].f = 0.5;        // порог косинусного сходства

    static Instruction code[] = {
        // --- ШАГ 1: Создаем изолированную ветку реальности ---
        { .operator_id = OP_SPAWN_CTX, .arg = {6} },

        // --- ШАГ 2: Ищем "прецедент" (аналогию) ---
        { .operator_id = OP_GET_NEIGHBORS, .arg = {1, 3, 0, 4} }, // src=1(A), rel=3(CAUSES), sp=0, count=4
        { .operator_id = OP_READ_SP, .arg = {10, 0} },            // R10 = sp[0] (должен быть B)
        { .operator_id = OP_FIND_SIMILAR, .arg = {10, 8, 20} },   // target=R10, thresh=R8(0.5), dst=R20 (C)

        // --- ШАГ 3: Ищем соседей аналога ---
        { .operator_id = OP_GET_NEIGHBORS, .arg = {20, 3, 30, 4} }, // src=R20(C), rel=3, sp=30, count=4
        { .operator_id = OP_READ_SP, .arg = {21, 30} },             // R21 = sp[30] (должен быть D)

        // --- ШАГ 4: Собираем путь для диагностики ---
        { .operator_id = OP_CONCAT_PATHS, .arg = {50, 1, 10, 20, 21} },

        // --- ШАГ 5: Формируем гипотезу (A CAUSES D) ---
        // OP_DERIVE: arg[0]=proc(3), arg[1]=A(1), arg[2]=D(21), arg[3]=cause(10 - прецедент), arg[4]=dst(9)
        { .operator_id = OP_DERIVE, .arg = {3, 1, 21, 10, 9} },

        // --- ШАГ 6: Схлопывание (порог < 0.4, чтобы пропустить гипотезу с confidence=0.4) ---
        { .operator_id = OP_MERGE_CTX, .arg = {0x3e4ccccd} }, // float 0.2f
        { .operator_id = OP_HALT }
    };

    Pipeline pipeline = { .code = code, .code_len = 10, .capacity = 10 };
    pipeline.constants.int_consts = NULL;
    pipeline.constants.int_count = 0;

    int rc = vm_execute(&ctx, &pipeline);
    assert(rc == VM_OK);

    char *result = (char*)(uintptr_t)ctx.scratchpad[50].value;
    assert(result != NULL);
    printf("Hypothesis path: %s\n", result);

    char expected[256];
    snprintf(expected, sizeof(expected), "Path(%lu -> %lu ~> %lu -> %lu)",
            (unsigned long)A, (unsigned long)B, (unsigned long)C, (unsigned long)D);
    assert(strcmp(result, expected) == 0);
    printf("Hypothesis analogy test passed.\n");

    free(result);
    vm_destroy(&ctx);
    hyper_memory_free(ctx.hyper_mem);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_hyp_db");

    return 0;
}
