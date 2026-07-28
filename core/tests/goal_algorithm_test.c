// tests/goal_algorithm_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "runtime/ops/vm_ops.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"
#include "knowledge/algorithm_loader.h"
#include "knowledge/algorithm_saver.h"
#include "reasoning/algorithm_planner.h"

int main(void) {
    system("rm -rf ./test_goal_db");
    assert(init_lmdb("./test_goal_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    // ----- инициализируем HyperMemory -----
    HyperMemory *hmem = hyper_memory_new(txn,
        db.graph.hyper.atoms,
        db.graph.hyper.idx_process,
        db.graph.hyper.idx_args,
        db.graph.hyper.idx_context);
    assert(hmem != NULL);

    // ----- цель -----
    uint64_t goal_id = djb2_hash("FindVulnerability");
    uint64_t algo_id = djb2_hash("CheckEdgeAlgo");

    // Сохраняем алгоритм в БД
    Instruction algo_code[] = {
        { .operator_id = OP_CHECK_CACHED_EDGE, .arg = {3, 0, 2, 1} },
        { .operator_id = OP_HALT }
    };
    Pipeline algo_pipeline = {
        .code = algo_code,
        .code_len = 2,
        .capacity = 2
    };
    assert(algorithm_save(txn, algo_id, &algo_pipeline) == MDB_SUCCESS);

    // ----- гипер-атом HAS_ALGORITHM -----
    NeuroAtom goal_algo_atom = {0};
    goal_algo_atom.id          = 1000 + algo_id;
    goal_algo_atom.process_id  = djb2_hash("HAS_ALGORITHM");
    goal_algo_atom.args[0].raw = HYPER_MAKE_REF(goal_id);
    goal_algo_atom.args[1].raw = HYPER_MAKE_REF(algo_id);
    goal_algo_atom.truth_mean       = 1.0f;
    goal_algo_atom.truth_confidence = 1.0f;
    int hrc = hyper_assert_unique(hmem, &goal_algo_atom);
    assert(hrc == 0 || hrc == 1);

    // ----- VM и операторы -----
    VMContext ctx;
    WorkingMemory wm_stub = {0};
    wm_init(&wm_stub, 100, 100);
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);
    ctx.hyper_mem = hmem;
    operator_registry_init();                // <-- ВАЖНО! Без этого операторы не зарегистрированы

    // Проверяем планировщик
    node_id_t selected_algo = 0;
    int rc = planner_select_algorithm(hmem, goal_id, &ctx, &selected_algo);
    assert(rc == 0);
    assert(selected_algo == algo_id);

    // ----- Бинарные рёбра (EDGE_FWD / EDGE_REV) -----
    uint64_t A = djb2_hash("A");
    uint64_t B = djb2_hash("B");
    uint64_t REL = djb2_hash("CAUSES");

    NeuroAtom fwd = {0};
    fwd.id = 2000;
    fwd.process_id = djb2_hash("EDGE_FWD");
    fwd.args[0].raw = HYPER_MAKE_REF(A);
    fwd.args[1].raw = HYPER_MAKE_REF(REL);
    fwd.truth_mean = 1.0f;
    fwd.truth_confidence = 1.0f;
    hyper_assert_unique(hmem, &fwd);

    NeuroAtom rev = {0};
    rev.id = 2001;
    rev.process_id = djb2_hash("EDGE_REV");
    rev.args[0].raw = HYPER_MAKE_REF(REL);
    rev.args[1].raw = HYPER_MAKE_REF(B);
    rev.truth_mean = 1.0f;
    rev.truth_confidence = 1.0f;
    hyper_assert_unique(hmem, &rev);

    // Активируем узлы в WM, чтобы load_context увидел их
    wm_activate(&wm_stub, A, 1.0f, 0.0f);
    wm_activate(&wm_stub, B, 1.0f, 0.0f);

    // Предзагрузка контекста
    Instruction load_ctx = { .operator_id = OP_LOAD_CONTEXT };
    rc = vm_op_load_context(&ctx, &load_ctx);
    assert(rc == VM_OK);

    // Устанавливаем регистры для CheckEdgeAlgo
    ctx.reg[0].type = REG_INT; ctx.reg[0].i = (int64_t)A;
    ctx.reg[1].type = REG_INT; ctx.reg[1].i = (int64_t)B;
    ctx.reg[2].type = REG_INT; ctx.reg[2].i = (int64_t)REL;
    ctx.reg[5].type = REG_INT; ctx.reg[5].i = (int64_t)selected_algo;

    // Выполняем алгоритм
    Instruction call = { .operator_id = OP_EXEC_ALGORITHM, .arg[0] = 5 };
    Pipeline outer = { .code = &call, .code_len = 1, .capacity = 1 };
    rc = vm_execute(&ctx, &outer);
    assert(rc == VM_OK);
    assert(ctx.reg[3].type == REG_BOOL);
    assert(ctx.reg[3].b == true);  // ребро A->B найдено

    printf("Goal algorithm test passed.\n");

    vm_destroy(&ctx);
    hyper_memory_free(hmem);
    mdb_txn_abort(txn);
    wm_clear(&wm_stub);
    close_lmdb();
    system("rm -rf ./test_goal_db");
    return 0;
}
