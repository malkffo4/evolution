// core/tests/abstraction_cup_test.c
//
// AGI OLYMPICS: ABSTRACTION CUP (TODO tests.txt, milestone 3 — ARC-like
// generalization). Domain 1: A --CAUSES--> B. Domain 2: C --CAUSES--> D.
// Единственная связь между доменами — векторное сходство B~C (НЕ имя,
// НЕ хэш, НЕ граф-путь). Система должна вывести A --CAUSES--> D — факт,
// которого нет НИГДЕ во входных данных ни напрямую, ни транзитивно.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/ops/opcode.h"
#include "runtime/ops/vm_ops.h"
#include "runtime/operator/operator.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/edge/edge.h"
#include "storage/graph/graph.h"
#include "storage/string_pool/string_pool.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "knowledge/knowledge_cache.h"
#include "math/hash.h"

int main(void) {
    system("rm -rf ./test_abstraction_db");
    assert(init_lmdb("./test_abstraction_db") == MDB_SUCCESS);
    operator_registry_init();

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    HyperMemory *hmem = hyper_memory_new(db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    assert(hmem != NULL);
    hyper_memory_set_db_causal(hmem, db.graph.hyper.idx_causal);

    uint64_t A   = djb2_hash("AbstractDomain1_EntityA");
    uint64_t B   = djb2_hash("AbstractDomain1_EntityB");
    uint64_t C   = djb2_hash("UnrelatedDomain2_EntityC");
    uint64_t D   = djb2_hash("UnrelatedDomain2_EntityD");
    uint64_t REL = djb2_hash("CAUSES");

    Node na = { .id = A, .name_hash = add_string_to_pool(txn, "EntityA"), .type = NODE_CONCEPT };
    Node nb = { .id = B, .name_hash = add_string_to_pool(txn, "EntityB"), .type = NODE_CONCEPT };
    Node nc = { .id = C, .name_hash = add_string_to_pool(txn, "EntityC"), .type = NODE_CONCEPT };
    Node nd = { .id = D, .name_hash = add_string_to_pool(txn, "EntityD"), .type = NODE_CONCEPT };
    assert(create_node(txn, &na) == MDB_SUCCESS);
    assert(create_node(txn, &nb) == MDB_SUCCESS);
    assert(create_node(txn, &nc) == MDB_SUCCESS);
    assert(create_node(txn, &nd) == MDB_SUCCESS);

    // Ровно два ребра во всей базе. Между {A,B} и {C,D} нет ни одного пути.
    Edge e1 = { .key = { A, REL, B }, .confidence = 1.0f, .evidence_count = 1 };
    Edge e2 = { .key = { C, REL, D }, .confidence = 1.0f, .evidence_count = 1 };
    assert(create_edge(txn, &e1) == MDB_SUCCESS);
    assert(create_edge(txn, &e2) == MDB_SUCCESS);

    // Единственный мост между доменами — векторное сходство, никогда граф.
    Vector128 emb_b = { .data = {1.0f, 0.1f, -0.3f} };
    Vector128 emb_c = { .data = {0.97f, 0.12f, -0.28f} };
    assert(hyper_vector_save(txn, db.graph.hyper.idx_vectors, B, &emb_b) == 0);
    assert(hyper_vector_save(txn, db.graph.hyper.idx_vectors, C, &emb_c) == 0);
    mdb_txn_commit(txn);

    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);
    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    WorkingMemory wm_stub = {0};
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);
    ctx.hyper_mem = hmem;
    ctx.current_episode_id = hyper_memory_new_id(hmem);

    assert(knowledge_cache_load_edges(&ctx, txn, A, REL) == MDB_SUCCESS);
    assert(knowledge_cache_load_edges(&ctx, txn, C, REL) == MDB_SUCCESS);

    // Шаг 1: что вызывает A?
    ctx.reg[1].type = REG_INT; ctx.reg[1].i = (int64_t)A;
    ctx.reg[3].type = REG_INT; ctx.reg[3].i = (int64_t)REL;
    Instruction get_n1 = { .operator_id = OP_GET_NEIGHBORS, .arg = {1, 3, 0, 7} };
    assert(vm_op_get_neighbors(&ctx, &get_n1) == VM_OK);
    assert(ctx.reg[7].i == 1);
    Instruction read1 = { .operator_id = OP_READ_SP, .arg = {4, 0} };
    assert(vm_op_read_sp(&ctx, &read1) == VM_OK);
    assert((uint64_t)ctx.reg[4].i == B);

    // Шаг 2: что структурно/семантически похоже на B, но в чужом домене?
    ctx.reg[8].type = REG_FLOAT; ctx.reg[8].f = 0.5;
    Instruction find_sim = { .operator_id = OP_FIND_SIMILAR, .arg = {4, 8, 5} };
    assert(vm_op_find_similar(&ctx, &find_sim) == VM_OK);
    assert((uint64_t)ctx.reg[5].node == C);

    // Шаг 3: что вызывает этот аналог?
    Instruction get_n2 = { .operator_id = OP_GET_NEIGHBORS, .arg = {5, 3, 30, 7} };
    assert(vm_op_get_neighbors(&ctx, &get_n2) == VM_OK);
    assert(ctx.reg[7].i == 1);
    Instruction read2 = { .operator_id = OP_READ_SP, .arg = {6, 30} };
    assert(vm_op_read_sp(&ctx, &read2) == VM_OK);
    assert((uint64_t)ctx.reg[6].i == D);

    // Шаг 4: перенести вывод обратно в домен 1: A --CAUSES--> D.
    ctx.reg[1].type = REG_NODE; ctx.reg[1].node = A;
    ctx.reg[6].type = REG_NODE; ctx.reg[6].node = D;
    ctx.reg[9].type = REG_INT;  ctx.reg[9].i = (int64_t)ctx.current_episode_id;
    Instruction derive = {0};
    derive.arg[0] = 3; derive.arg[1] = 1; derive.arg[2] = 6; derive.arg[3] = 9; derive.arg[4] = 20;
    derive.operator_id = OP_DERIVE;
    assert(vm_op_derive(&ctx, &derive) == VM_OK);

    node_id_t hyp_id = (node_id_t)ctx.reg[20].i;
    assert(hyp_id != 0);

    NeuroAtom stored;
    MDB_val key = { sizeof(node_id_t), &hyp_id };
    MDB_val data;
    assert(mdb_get(txn, hmem->dbi_atoms, &key, &data) == MDB_SUCCESS);
    memcpy(&stored, data.mv_data, sizeof(NeuroAtom));
    assert(stored.process_id == REL);
    assert(HYPER_GET_ID(stored.args[0].raw) == A);
    assert(HYPER_GET_ID(stored.args[1].raw) == D);
    assert(stored.truth_confidence < 0.6f); // это гипотеза, не факт

    EdgeList from_a = {0};
    assert(get_edges_from_node(txn, A, &from_a) == MDB_SUCCESS);
    for (uint32_t i = 0; i < from_a.count; i++)
        assert(from_a.items[i].key.target != D);   // прямого пути A->D нет
    free(from_a.items);

    printf("[Abstraction Cup] A --CAUSES--> B ~ C --CAUSES--> D\n");
    printf("[Abstraction Cup] Перенесённая гипотеза id=%lu: A --CAUSES--> D "
           "(confidence=%.2f, отсутствует в исходном графе)\n",
           (unsigned long)hyp_id, stored.truth_confidence);
    printf("[OK] Структурный паттерн из Domain 1 абстрагирован и корректно "
           "перенесён в Domain 2 через чистую векторную аналогию.\n");

    vm_destroy(&ctx);
    hyper_memory_free(hmem);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_abstraction_db");
    return 0;
}
