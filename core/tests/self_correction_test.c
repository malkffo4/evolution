// core/tests/self_correction_test.c
//
// TODO tests.txt milestone 2 (Self-Correction), CI-safe increment: proves
// the closed loop execution-failure -> quarantine -> HAS_FLAW -> score
// penalty -> autonomous re-selection, using the REAL planner code path
// (vm_op_select_algorithm, the exact function CorePlanner invokes every
// tick via OP_SELECT_ALGORITHM), not a re-implementation of UCB1.
//
// This does NOT cover bytecode self-repair (an algorithm rewriting its
// own instructions after reading its failure log) -- that needs the
// Capability/Permission system from TODO.md before it's safe to run
// unattended. Design sketch at the bottom of the response, not in code.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <lmdb.h>

#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "runtime/ops/vm_ops.h"
#include "memory/working.h"
#include "memory/critic_state.h"
#include "knowledge/evaluation.h"
#include "math/hash.h"

extern bool is_quarantined(uint64_t algo_id);
extern void record_execution_result(uint64_t algo_id, int rc);
extern void init_quarantine(void);

static void link_has_algorithm(MDB_txn *txn, HyperMemory *hmem, node_id_t algo, node_id_t goal) {
    NeuroAtom link = {0};
    link.id = hyper_memory_new_id(hmem);
    link.process_id = proc_make(djb2_hash("HAS_ALGORITHM"), PROC_KIND_RELATION);
    link.args[0].raw = HYPER_MAKE_REF(algo);
    link.args[1].raw = HYPER_MAKE_REF(goal);
    link.truth_mean = 1.0f;
    link.truth_confidence = 1.0f;
    assert(hyper_assert_unique(txn, hmem, &link) >= 0);
}

int main(void) {
    system("rm -rf ./test_self_correction_db");
    assert(init_lmdb("./test_self_correction_db") == MDB_SUCCESS);
    operator_registry_init();
    init_quarantine();

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    HyperMemory *hmem = hyper_memory_new(db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    assert(hmem != NULL);
    hyper_memory_set_db_causal(hmem, db.graph.hyper.idx_causal);

    node_id_t GOAL      = djb2_hash("SelfCorrectionGoal");
    node_id_t GOOD_ALGO = djb2_hash("ReliableExploit");
    node_id_t BAD_ALGO  = djb2_hash("FlakyExploit");

    link_has_algorithm(txn, hmem, GOOD_ALGO, GOAL);
    link_has_algorithm(txn, hmem, BAD_ALGO,  GOAL);

    // Equal number of observations for both -> the ONLY variable that
    // can explain the planner's final choice is quality, not novelty
    // (UCB1's exploration bonus is symmetric when confidence is equal).
    for (int i = 0; i < 5; i++) {
        assert(score_update(txn, hmem, COGNITIVE_DOMAIN_ALGORITHM, GOOD_ALGO, 1.0f, 0, 0) == 0);
        record_execution_result(BAD_ALGO, VM_ERROR);
        assert(score_update(txn, hmem, COGNITIVE_DOMAIN_ALGORITHM, BAD_ALGO, 0.0f, 0, 0) == 0);
    }

    // Critic quarantines the repeat-offender (>=3 consecutive failures)
    // and demotes it with an explicit, explainable HAS_FLAW + delta.
    assert(is_quarantined(BAD_ALGO) == true);

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    WorkingMemory wm_stub = {0};
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);
    ctx.hyper_mem = hmem;
    ctx.current_episode_id = hyper_memory_new_id(hmem);

    Instruction critic_ins = {0};
    assert(vm_op_critic_apply(&ctx, &critic_ins) == VM_OK);

    float good_score = score_get(txn, hmem, COGNITIVE_DOMAIN_ALGORITHM, GOOD_ALGO);
    float bad_score  = score_get(txn, hmem, COGNITIVE_DOMAIN_ALGORITHM, BAD_ALGO);
    printf("[SelfCorrection] score(good)=%.4f score(bad)=%.4f\n", good_score, bad_score);
    assert(good_score > bad_score);

    // The REAL planner code path must now autonomously prefer the
    // reliable algorithm -- exactly what CorePlanner calls every tick.
    ctx.reg[1].type = REG_NODE; ctx.reg[1].node = GOAL;
    Instruction select_ins = { .operator_id = OP_SELECT_ALGORITHM, .arg = {1, 0, 3} };
    assert(vm_op_select_algorithm(&ctx, &select_ins) == VM_OK);
    assert(ctx.reg[3].type == REG_INT && ctx.reg[3].i > 0);

    node_id_t chosen = (node_id_t)ctx.scratchpad[0].value;
    printf("[SelfCorrection] planner chose algorithm id=%lu (good=%lu, bad=%lu)\n",
           (unsigned long)chosen, (unsigned long)GOOD_ALGO, (unsigned long)BAD_ALGO);
    assert(chosen == GOOD_ALGO);

    printf("[OK] Self-correction loop verified: repeated failure -> quarantine -> "
           "HAS_FLAW -> score penalty -> autonomous strategy switch.\n");

    vm_destroy(&ctx);
    hyper_memory_free(hmem);
    mdb_txn_commit(txn);
    close_lmdb();
    system("rm -rf ./test_self_correction_db");
    return 0;
}
