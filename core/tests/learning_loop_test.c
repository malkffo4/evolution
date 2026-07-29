// tests/learning_loop_test.c
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
#include "knowledge/evaluation.h"
#include "math/hash.h"

/*
 * Проверяет цепочку:
 *   OP_DERIVE создаёт гипотезу с причинностью
 *   -> ctx->last_result_id указывает на созданный атом
 *   -> score_propagate_credit() распространяет credit по idx_causal
 *   -> Score всех участников цепочки меняется согласно outcome и глубине
 *
 * Это именно тот механизм, который теперь встроен в vm_op_evaluate_goals()
 * и будет автоматически срабатывать в реальном MainLoop.
 */
int main(void) {
    system("rm -rf ./test_learning_db");
    assert(init_lmdb("./test_learning_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    HyperMemory *hmem = hyper_memory_new(txn,
        db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    assert(hmem != NULL);
    hyper_memory_set_db_causal(hmem, db.graph.hyper.idx_causal);

    node_id_t BIRD = djb2_hash("BIRD");
    node_id_t CAN  = djb2_hash("CAN");
    node_id_t FLY  = djb2_hash("FLY");
    node_id_t BAT  = djb2_hash("BAT");

    // Прецедент в базовой реальности: BIRD -CAN-> FLY, cause=0
    NeuroAtom precedent = {0};
    precedent.id = 100;
    precedent.process_id = CAN;
    precedent.args[0].raw = HYPER_MAKE_REF(BIRD);
    precedent.args[1].raw = HYPER_MAKE_REF(FLY);
    precedent.truth_mean = 1.0f;
    precedent.truth_confidence = 1.0f;
    assert(hyper_assert_with_cause(hmem, &precedent, 0) >= 0);

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    WorkingMemory wm_stub = {0};
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);
    operator_registry_init();
    ctx.hyper_mem = hmem;
    ctx.current_episode_id = 999;

    assert(ctx.last_result_id == 0); // нулевая инициализация через memset в vm_init

    // Гипотеза по аналогии: BAT -CAN-> FLY, cause = precedent.id
    ctx.reg[1].type = REG_INT; ctx.reg[1].i = (int64_t)CAN;
    ctx.reg[2].type = REG_INT; ctx.reg[2].i = HYPER_MAKE_REF(BAT);
    ctx.reg[3].type = REG_INT; ctx.reg[3].i = HYPER_MAKE_REF(FLY);
    ctx.reg[4].type = REG_INT; ctx.reg[4].i = (int64_t)precedent.id;

    Instruction derive = {0};
    derive.arg[0]=1; derive.arg[1]=2; derive.arg[2]=3; derive.arg[3]=4; derive.arg[4]=9;
    int rc = vm_op_derive(&ctx, &derive);
    assert(rc == VM_OK);

    node_id_t hypothesis_id = (node_id_t)ctx.reg[9].i;
    assert(hypothesis_id != 0);
    assert(ctx.last_result_id == hypothesis_id); // канал сработал

    float before_bird = score_get(hmem, COGNITIVE_DOMAIN_HYPOTHESIS, BIRD);
    float before_bat  = score_get(hmem, COGNITIVE_DOMAIN_HYPOTHESIS, BAT);
    assert(before_bird == SCORE_PRIOR);
    assert(before_bat  == SCORE_PRIOR);

    // Симулируем то, что делает vm_op_evaluate_goals() после успеха
    int propagated = score_propagate_credit(hmem, COGNITIVE_DOMAIN_HYPOTHESIS,
                                             ctx.last_result_id, 1.0f, 0, 0.7f);
    assert(propagated == 4); // 2 атома на цепочке * 2 REF-аргумента
    ctx.last_result_id = 0;  // именно так поступает vm_op_evaluate_goals

    float after_bat  = score_get(hmem, COGNITIVE_DOMAIN_HYPOTHESIS, BAT);   // depth 0
    float after_bird = score_get(hmem, COGNITIVE_DOMAIN_HYPOTHESIS, BIRD);  // depth 1 (причина)

    printf("BAT score=%.4f (depth 0), BIRD score=%.4f (depth 1)\n", after_bat, after_bird);

    assert(after_bat  > before_bat);
    assert(after_bird > before_bird);
    // Ближний участник (discount^0=1.0) учится быстрее дальнего (discount^1=0.7)
    assert(after_bat > after_bird);
    assert(ctx.last_result_id == 0); // сброс не даёт "утечь" в следующий запуск

    mdb_txn_commit(txn);
    vm_destroy(&ctx);
    hyper_memory_free(hmem);
    close_lmdb();
    system("rm -rf ./test_learning_db");
    printf("Learning loop test passed: VM -> last_result_id -> credit propagation wired correctly.\n");
    return 0;
}
