// tests/episode_integration_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdbool.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "runtime/ops/vm_ops.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/db/db_writer.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"
#include "knowledge/algorithm_saver.h"
#include "knowledge/episode.h"
#include "knowledge/evaluation.h"

int main(void) {
    system("rm -rf ./test_episode_int_db");
    assert(init_lmdb("./test_episode_int_db") == MDB_SUCCESS);
    assert(db_writer_start() == 0); // Запускаем writer-поток для фоновых воркеров

    node_id_t goal_id = djb2_hash("IntegrationGoal");
    node_id_t algo_id = djb2_hash("IntegrationAlgo");

    // --- ФАЗА 1: НАСТРОЙКА БАЗЫ (Write Transaction) ---
    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    HyperMemory *hmem = hyper_memory_new(txn,
        db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    assert(hmem != NULL);

    Instruction code[] = {
        { .operator_id = OP_LOAD_CONST, .arg = {1, 2} },
        { .operator_id = OP_LOAD_CONST, .arg = {2, 3} },
        { .operator_id = OP_ADD,        .arg = {0, 1, 2} },
        { .operator_id = OP_HALT }
    };
    Pipeline pipeline = { .code = code, .code_len = 4, .capacity = 4 };
    assert(algorithm_save(txn, algo_id, &pipeline) == MDB_SUCCESS);

    NeuroAtom meta = {0};
    meta.id = 6000;
    meta.process_id = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
    meta.args[0].raw = djb2_hash("HAS_ALGORITHM");
    meta.args[1].raw = djb2_hash("GoalAlgorithmRelation");
    meta.truth_mean = 1.0f; meta.truth_confidence = 1.0f;
    assert(hyper_assert_unique(hmem, &meta) >= 0);

    NeuroAtom link = {0};
    link.id = 5000;
    link.process_id = proc_make(djb2_hash("HAS_ALGORITHM"), PROC_KIND_RELATION);
    link.args[0].raw = HYPER_MAKE_REF(goal_id);
    link.args[1].raw = HYPER_MAKE_REF(algo_id);
    link.truth_mean = 1.0f; link.truth_confidence = 1.0f;
    assert(hyper_assert_unique(hmem, &link) >= 0);

    hyper_memory_free(hmem);
    assert(mdb_txn_commit(txn) == 0); // ОБЯЗАТЕЛЬНО коммитим, чтобы освободить лок писателя!


    // --- ФАЗА 2: ДИСПЕТЧЕРИЗАЦИЯ (Read-Only Transaction) ---
    MDB_txn *read_txn;
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &read_txn) == 0);

    HyperMemory *read_hmem = hyper_memory_new(read_txn,
        db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    WorkingMemory wm;
    assert(wm_init(&wm, 16, 16) == 0);
    assert(vm_init(&ctx, read_txn, &wm) == VM_OK);

    operator_registry_init();
    ctx.hyper_mem = read_hmem;

    wm_activate(&wm, goal_id, 1.0f, 0.0f);
    for (uint32_t i = 0; i < wm.count; i++)
        if (wm.nodes[i].node_id == goal_id) wm.nodes[i].state.usefulness = 0.9f;

    Instruction eval_ins = { .operator_id = OP_EVALUATE_GOALS };
    int rc = vm_op_evaluate_goals(&ctx, &eval_ins);
    printf("vm_op_evaluate_goals rc=%d (VM_OK=%d, VM_NOT_FOUND=%d)\n", rc, VM_OK, VM_NOT_FOUND);
    assert(rc == VM_OK);

    vm_destroy(&ctx);

    // ИСПРАВЛЕНИЕ УТЕЧКИ: освобождаем hmem, созданный для read-only транзакции
    hyper_memory_free(read_hmem);

    mdb_txn_abort(read_txn); // Закрываем RO транзакцию


    // --- ФАЗА 3: ОЖИДАНИЕ РЕЗУЛЬТАТА (Асинхронный поллинг) ---
    bool found = false;
    float score = 0.0f;
    ko_id_t found_episode_id = 0;
    node_id_t episode_proc = proc_make(djb2_hash("EPISODE_RECORDED"), PROC_KIND_EVENT);

    for (int attempt = 0; attempt < 50 && !found; attempt++) {
        usleep(20000); // 20ms

        MDB_txn *poll_txn;
        if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &poll_txn) != 0) continue;

        HyperMemory *poll_hmem = hyper_memory_new(poll_txn,
            db.graph.hyper.atoms, db.graph.hyper.idx_process,
            db.graph.hyper.idx_args, db.graph.hyper.idx_context);

        score = score_get(poll_hmem, COGNITIVE_DOMAIN_ALGORITHM, algo_id);

        if (score > SCORE_PRIOR) {
            NeuroAtom *episodes = NULL;
            size_t count = 0;
            if (hyper_find_by_participant(poll_hmem, goal_id, 0, &episodes, &count) == 0) {
                for (size_t i = 0; i < count; i++) {
                    if (episodes[i].process_id == episode_proc) {
                        found_episode_id = episodes[i].id;
                        found = true;
                        break;
                    }
                }
                free(episodes);
            }
        }
        hyper_memory_free(poll_hmem);
        mdb_txn_abort(poll_txn);
    }

    assert(found && "Async worker did not update score and episode in time");
    assert(score > SCORE_PRIOR);

    MDB_txn *final_txn;
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &final_txn) == 0);
    Episode ep;
    assert(episode_load(final_txn, found_episode_id, &ep) == MDB_SUCCESS);
    assert(ep.goal_id == goal_id && ep.algorithm_id == algo_id && ep.vm_status == VM_OK);
    assert(ep.outcome == 1.0f);
    mdb_txn_abort(final_txn);

    printf("Episode integration test passed: id=%lu duration_cycles=%lu\n",
           (unsigned long)ep.id, (unsigned long)ep.duration_cycles);

    wm_clear(&wm);
    db_writer_stop();
    close_lmdb();
    system("rm -rf ./test_episode_int_db");
    return 0;
}
