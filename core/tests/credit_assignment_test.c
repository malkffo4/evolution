// tests/credit_assignment_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <lmdb.h>

#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "knowledge/evaluation.h"
#include "math/hash.h"

int main(void) {
    system("rm -rf ./test_credit_db");
    assert(init_lmdb("./test_credit_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    HyperMemory *hmem = hyper_memory_new(txn,
        db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    assert(hmem);
    hyper_memory_set_db_causal(hmem, db.graph.hyper.idx_causal);

    node_id_t FIRE  = djb2_hash("FIRE");
    node_id_t SMOKE = djb2_hash("SMOKE");
    node_id_t ALARM = djb2_hash("ALARM");
    node_id_t CAUSES = djb2_hash("CAUSES");

    // depth2: FIRE -CAUSES-> SMOKE (корневое утверждение, cause=0)
    NeuroAtom a10 = {0};
    a10.id = 10; a10.process_id = CAUSES;
    a10.args[0].raw = HYPER_MAKE_REF(FIRE);
    a10.args[1].raw = HYPER_MAKE_REF(SMOKE);
    a10.truth_mean = 1.0f; a10.truth_confidence = 1.0f;
    assert(hyper_assert_with_cause(hmem, &a10, 0) >= 0);

    // depth1: SMOKE -CAUSES-> ALARM, cause = a10
    NeuroAtom a11 = {0};
    a11.id = 11; a11.process_id = CAUSES;
    a11.args[0].raw = HYPER_MAKE_REF(SMOKE);
    a11.args[1].raw = HYPER_MAKE_REF(ALARM);
    a11.truth_mean = 1.0f; a11.truth_confidence = 1.0f;
    assert(hyper_assert_with_cause(hmem, &a11, a10.id) >= 0);

    // depth0: DERIVE FIRE -CAUSES-> ALARM, cause = a11
    NeuroAtom a12 = {0};
    a12.id = 12; a12.process_id = CAUSES;
    a12.args[0].raw = HYPER_MAKE_REF(FIRE);
    a12.args[1].raw = HYPER_MAKE_REF(ALARM);
    a12.truth_mean = 1.0f; a12.truth_confidence = 0.4f;
    assert(hyper_assert_with_cause(hmem, &a12, a11.id) >= 0);

    float before_fire  = score_get(hmem, COGNITIVE_DOMAIN_HYPOTHESIS, FIRE);
    float before_smoke  = score_get(hmem, COGNITIVE_DOMAIN_HYPOTHESIS, SMOKE);
    float before_alarm  = score_get(hmem, COGNITIVE_DOMAIN_HYPOTHESIS, ALARM);
    assert(before_fire == SCORE_PRIOR && before_smoke == SCORE_PRIOR && before_alarm == SCORE_PRIOR);

    int propagated = score_propagate_credit(hmem, COGNITIVE_DOMAIN_HYPOTHESIS,
                                             a12.id, 1.0f, 8, 0.5f);
    assert(propagated == 6); // 3 узла на цепочке * 2 REF-аргумента

    float fire  = score_get(hmem, COGNITIVE_DOMAIN_HYPOTHESIS, FIRE);
    float smoke = score_get(hmem, COGNITIVE_DOMAIN_HYPOTHESIS, SMOKE);
    float alarm = score_get(hmem, COGNITIVE_DOMAIN_HYPOTHESIS, ALARM);

    printf("FIRE=%.4f SMOKE=%.4f ALARM=%.4f\n", fire, smoke, alarm);

    // Все выросли относительно приора
    assert(fire  > SCORE_PRIOR);
    assert(smoke > SCORE_PRIOR);
    assert(alarm > SCORE_PRIOR);

    // Аналитически предсказанные значения (см. обоснование в PR-описании)
    // БЫЛО:
    // assert(fabsf(fire  - 0.62f)  < 0.01f);
    // assert(fabsf(smoke - 0.5725f) < 0.01f);
    // assert(fabsf(alarm - 0.64f)  < 0.01f);

    // СТАЛО: аналитика под реальную Beta-Bernoulli формулу score_update_weighted()
    // (kappa=6.0 для COGNITIVE_DOMAIN_HYPOTHESIS, приор Beta(0.5,0.5),
    // пересобираемый из (truth_mean, truth_confidence) на каждом вызове —
    // см. evaluation.c). Два обновления на узел:
    //   FIRE:  w=1.0 (depth0) затем w=0.25 (depth2) -> 2/3
    //   SMOKE: w=0.5 (depth1) затем w=0.25 (depth2) -> 13/21
    //   ALARM: w=1.0 (depth0) затем w=0.5  (depth1) -> 7/10
    assert(fabsf(fire  - (2.0f/3.0f))  < 0.001f);
    assert(fabsf(smoke - (13.0f/21.0f)) < 0.001f);
    assert(fabsf(alarm - 0.7f)          < 0.001f);

    // ALARM встречается на ДВУХ хопах (depth0 и depth1) -> получил больше
    // суммарного credit, чем FIRE и SMOKE, каждый из которых тоже на двух,
    // но с более глубоким (слабым) вторым вкладом.
    assert(alarm > fire);
    assert(alarm > smoke);

    // Пустая/битая цепочка не должна падать
    assert(score_propagate_credit(hmem, COGNITIVE_DOMAIN_HYPOTHESIS, 0, 1.0f, 8, 0.5f) == -1);
    assert(score_propagate_credit(NULL, COGNITIVE_DOMAIN_HYPOTHESIS, a12.id, 1.0f, 8, 0.5f) == -1);

    mdb_txn_commit(txn);
    hyper_memory_free(hmem);
    close_lmdb();
    system("rm -rf ./test_credit_db");
    printf("Credit assignment test passed.\n");
    return 0;
}
