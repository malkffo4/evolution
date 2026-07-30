// tests/episode_integration_test.c
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
#include "knowledge/algorithm_saver.h"
#include "knowledge/episode.h"
#include "knowledge/evaluation.h"

/*
 * Проверяет ПОЛНУЮ интеграцию: WM-активная цель -> wm_get_highest_goal
 * -> planner_select_algorithm -> vm_execute -> score_update -> episode_record,
 * то есть реальную точку интеграции в cognitive.c, а не изолированный
 * модуль (см. episode_test.c). Использует те же "сырые" process_id, что и
 * find_goal_algorithm_relations()/find_algorithms_for_goal() ПОСЛЕ фикса
 * (proc_make(hash, PROC_KIND_RELATION)) — согласовано с реальным кодом.
 */
int main(void) {
    system("rm -rf ./test_episode_int_db");
    assert(init_lmdb("./test_episode_int_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    HyperMemory *hmem = hyper_memory_new(txn,
        db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    assert(hmem != NULL);

    node_id_t goal_id = djb2_hash("IntegrationGoal");
    node_id_t algo_id = djb2_hash("IntegrationAlgo");

    // Гарантированно успешный алгоритм: 2+3, HALT.
    Instruction code[] = {
        { .operator_id = OP_LOAD_CONST, .arg = {1, 2} },
        { .operator_id = OP_LOAD_CONST, .arg = {2, 3} },
        { .operator_id = OP_ADD,        .arg = {0, 1, 2} },
        { .operator_id = OP_HALT }
    };
    Pipeline pipeline = { .code = code, .code_len = 4, .capacity = 4 };
    assert(algorithm_save(txn, algo_id, &pipeline) == MDB_SUCCESS);

    // Meta: IS_A(HAS_ALGORITHM, GoalAlgorithmRelation) — нужно для
    // wm_get_highest_goal()'s внутренней проверки is_goal.
    NeuroAtom meta = {0};
    meta.id = 6000;
    meta.process_id = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
    meta.args[0].raw = djb2_hash("HAS_ALGORITHM");
    meta.args[1].raw = djb2_hash("GoalAlgorithmRelation");
    meta.truth_mean = 1.0f; meta.truth_confidence = 1.0f;
    assert(hyper_assert_unique(hmem, &meta) >= 0);

    // HAS_ALGORITHM(goal_id, algo_id)
    NeuroAtom link = {0};
    link.id = 5000;
    link.process_id = proc_make(djb2_hash("HAS_ALGORITHM"), PROC_KIND_RELATION);
    link.args[0].raw = HYPER_MAKE_REF(goal_id);
    link.args[1].raw = HYPER_MAKE_REF(algo_id);
    link.truth_mean = 1.0f; link.truth_confidence = 1.0f;
    assert(hyper_assert_unique(hmem, &link) >= 0);

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    WorkingMemory wm;
    assert(wm_init(&wm, 16, 16) == 0);
    assert(vm_init(&ctx, txn, &wm) == VM_OK);
    operator_registry_init();
    ctx.hyper_mem = hmem;

    wm_activate(&wm, goal_id, 1.0f, 0.0f);
    for (uint32_t i = 0; i < wm.count; i++)
        if (wm.nodes[i].node_id == goal_id) wm.nodes[i].state.usefulness = 0.9f;

    Instruction eval_ins = { .operator_id = OP_EVALUATE_GOALS };
    int rc = vm_op_evaluate_goals(&ctx, &eval_ins);
    printf("vm_op_evaluate_goals rc=%d (VM_OK=%d, VM_NOT_FOUND=%d)\n", rc, VM_OK, VM_NOT_FOUND);
    assert(rc == VM_OK);

    float score = score_get(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo_id);
    assert(score > SCORE_PRIOR);

    NeuroAtom *episodes = NULL;
    size_t count = 0;
    assert(hyper_find_by_participant(hmem, goal_id, 0, &episodes, &count) == 0);

    node_id_t episode_proc = proc_make(djb2_hash("EPISODE_RECORDED"), PROC_KIND_EVENT);
    ko_id_t found_episode_id = 0;
    for (size_t i = 0; i < count; i++) {
        if (episodes[i].process_id == episode_proc) {
            found_episode_id = episodes[i].id;
            assert(HYPER_GET_ID(episodes[i].args[0].raw) == goal_id);
            assert(HYPER_GET_ID(episodes[i].args[1].raw) == algo_id);
            assert(episodes[i].truth_mean == 1.0f);
        }
    }
    free(episodes);
    assert(found_episode_id != 0);

    Episode ep;
    assert(episode_load(txn, found_episode_id, &ep) == MDB_SUCCESS);
    assert(ep.goal_id == goal_id && ep.algorithm_id == algo_id && ep.vm_status == VM_OK);
    assert(ep.outcome == 1.0f);

    printf("Episode integration test passed: id=%lu duration_cycles=%lu\n",
           (unsigned long)ep.id, (unsigned long)ep.duration_cycles);

    vm_destroy(&ctx);
    wm_clear(&wm);
    hyper_memory_free(hmem);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_episode_int_db");
    return 0;
}
