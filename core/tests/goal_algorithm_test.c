// tests/goal_algorithm_test.c
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
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"
#include "knowledge/knowledge_cache.h"
#include "knowledge/algorithm_loader.h"
#include "knowledge/algorithm_saver.h"
#include "reasoning/algorithm_planner.h"

// WorkingMemory global_wm;
// volatile sig_atomic_t g_running = 1;

int main(void) {
    system("rm -rf ./test_goal_db");
    assert(init_lmdb("./test_goal_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    // 1. Цель и алгоритм
    uint64_t goal_id = djb2_hash("FindVulnerability");
    Node goal_node = { .id = goal_id, .name_hash = add_string_to_pool(txn, "FindVulnerability"), .type = NODE_GOAL };
    assert(create_node(txn, &goal_node) == MDB_SUCCESS);

    uint64_t algo_id = djb2_hash("CheckEdgeAlgo");

    Instruction algo_code[] = {
        { .operator_id = OP_CHECK_CACHED_EDGE, .arg[0] = 3, .arg[1] = 0, .arg[2] = 2, .arg[3] = 1 },
        { .operator_id = OP_HALT }
    };
    Pipeline algo_pipeline;
    algo_pipeline.code = algo_code;
    algo_pipeline.code_len = 2;
    algo_pipeline.capacity = 2;
    algo_pipeline.constants.int_consts = NULL;
    algo_pipeline.constants.int_count = 0;

    assert(algorithm_save(txn, algo_id, &algo_pipeline) == MDB_SUCCESS);

    uint64_t rel_has_algo = djb2_hash("HAS_ALGORITHM");
    Edge link = { .key = { goal_id, rel_has_algo, algo_id }, .confidence = 1.0f, .evidence_count = 1 };
    assert(create_edge(txn, &link) == MDB_SUCCESS);

    // Гипер-атом для планировщика
    HyperMemory *hmem = hyper_memory_new(txn,
        db.graph.hyper.atoms,
        db.graph.hyper.idx_process,
        db.graph.hyper.idx_args,
        db.graph.hyper.idx_context);

    HyperAtom goal_algo_atom = {
        .id = 1000 + algo_id,
        .process_id = rel_has_algo,
        .args = {
            { .raw = HYPER_MAKE_REF(goal_id) },
            { .raw = HYPER_MAKE_REF(algo_id) },
            { .raw = 0 }
        },
        .context_id = 0,
        .time_tick = 0,
        .cause_id = 0
    };
    int hrc = hyper_assert_unique(hmem, &goal_algo_atom);
    assert(hrc == 0);

    // 2. Тестовые данные
    uint64_t A = djb2_hash("A");
    uint64_t B = djb2_hash("B");
    uint64_t REL = djb2_hash("CAUSES");
    Node na = { .id = A, .name_hash = add_string_to_pool(txn, "A"), .type = NODE_CONCEPT };
    Node nb = { .id = B, .name_hash = add_string_to_pool(txn, "B"), .type = NODE_CONCEPT };
    assert(create_node(txn, &na) == MDB_SUCCESS);
    assert(create_node(txn, &nb) == MDB_SUCCESS);
    Edge e = { .key = { A, REL, B }, .confidence = 1.0f, .evidence_count = 1 };
    assert(create_edge(txn, &e) == MDB_SUCCESS);

    // 3. VM и планировщик (всё в одной транзакции)
    VMContext ctx;
    WorkingMemory wm_stub = {0};
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);
    operator_registry_init();

    node_id_t selected_algo = 0;
    int rc = planner_select_algorithm(hmem, goal_id, &ctx, &selected_algo);
    assert(rc == 0);
    assert(selected_algo == algo_id);

    assert(knowledge_cache_load_edges(&ctx, txn, A, REL) == MDB_SUCCESS);

    ctx.reg[0].type = REG_INT; ctx.reg[0].i = (int64_t)A;
    ctx.reg[1].type = REG_INT; ctx.reg[1].i = (int64_t)B;
    ctx.reg[2].type = REG_INT; ctx.reg[2].i = (int64_t)REL;

    ctx.reg[5].type = REG_INT; ctx.reg[5].i = (int64_t)selected_algo;

    Instruction call = { .operator_id = OP_EXEC_ALGORITHM, .arg[0] = 5 };
    Pipeline outer = { .code = &call, .code_len = 1, .capacity = 1 };
    outer.constants.int_consts = NULL;
    outer.constants.int_count = 0;

    rc = vm_execute(&ctx, &outer);
    if (rc != VM_OK) {
        printf("vm_execute failed with status %d\n", rc);
        // Дополнительно: проверим, загружается ли алгоритм вручную
        Pipeline *test_load = NULL;
        int load_rc = algorithm_load(ctx.memory.txn, algo_id, &test_load);
        printf("algorithm_load returned %d\n", load_rc);
        if (test_load) {
            printf("Loaded pipeline code_len=%u\n", test_load->code_len);
            free(test_load->code);
            free(test_load);
        }
    }
    assert(rc == VM_OK);

    assert(ctx.reg[3].type == REG_BOOL);
    assert(ctx.reg[3].b == true);

    printf("Goal algorithm test passed (VM clean, planner in Virtual Mind).\n");

    vm_destroy(&ctx);
    hyper_memory_free(hmem);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_goal_db");
    return 0;
}
