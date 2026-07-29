// tests/evaluation_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <lmdb.h>
#include <unistd.h>
#include <math.h>

#include "memory/decay.h"
#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/vm_ops.h"
#include "runtime/ops/opcode.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"
#include "knowledge/evaluation.h"

int main(void) {
    system("rm -rf ./test_eval_db");
    assert(init_lmdb("./test_eval_db") == MDB_SUCCESS);

    // ---------- Этап 1: оценка алгоритмов через score_update ----------
    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    MDB_dbi atoms       = db.graph.hyper.atoms;
    MDB_dbi idx_proc    = db.graph.hyper.idx_process;
    MDB_dbi idx_args    = db.graph.hyper.idx_args;
    MDB_dbi idx_ctx     = db.graph.hyper.idx_context;
    MDB_dbi idx_causal  = db.graph.hyper.idx_causal;
    MDB_dbi archive     = db.graph.hyper.archive;
    MDB_dbi idx_vectors = db.graph.hyper.idx_vectors;

    HyperMemory *hmem = hyper_memory_new(txn, atoms, idx_proc, idx_args, idx_ctx);
    assert(hmem != NULL);
    hyper_memory_set_db_causal(hmem, idx_causal);
    hyper_memory_set_db_archive(hmem, archive);
    hyper_memory_set_db_vectors(hmem, idx_vectors);

    node_id_t algo1 = djb2_hash("algo_sort");
    node_id_t algo2 = djb2_hash("algo_search");

    // 1. Без наблюдений возвращается нейтральный приор
    float s1 = score_get(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1);
    assert(s1 == SCORE_PRIOR);

    // 2. Пять успешных запусков – доверие растёт
    for (int i = 0; i < 5; i++) {
        int rc = score_update(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1,
                              1.0f,   // outcome = success
                              0, 0);  // cause и context пока не используются
        assert(rc == 0);
    }
    float s1_after_success = score_get(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1);
    assert(s1_after_success > SCORE_PRIOR);
    printf("After 5 successes: score = %.3f\n", s1_after_success);

    // 3. Одна неудача – доверие падает
    score_update(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1, 0.0f, 0, 0);
    float s1_after_fail = score_get(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1);
    assert(s1_after_fail < s1_after_success);
    printf("After 1 failure:  score = %.3f\n", s1_after_fail);

    // 4. algo2 получает одно успешное наблюдение – доверие тоже выше приора
    score_update(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo2, 1.0f, 0, 0);
    float s2 = score_get(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo2);
    assert(s2 > SCORE_PRIOR);
    assert(isfinite(s1_after_fail));
    assert(isfinite(s2));
    printf("Score for algo2:  %.3f\n", s2);

    // 5. Проверяем, что Score‑атом находится через hyper_find_by_participant
    NeuroAtom *results = NULL;
    size_t count = 0;
    int rc = hyper_find_by_participant(hmem, algo1, 0, &results, &count);
    assert(rc == 0);
    int found_score = 0;
    for (size_t i = 0; i < count; i++) {
        if (results[i].process_id == proc_make(djb2_hash("HAS_SCORE"), PROC_KIND_RELATION)) {
            found_score++;
            assert(HYPER_GET_ID(results[i].args[0].raw) == algo1);
            assert((results[i].args[1].raw & HYPER_TYPE_MASK) == HYPER_TYPE_INT);
        }
    }
    assert(found_score == 1);
    free(results);
    printf("HAS_SCORE atom found via participant index.\n");

    // 6. Проверяем, что Evaluation‑атомы тоже созданы (хотя бы один)
    results = NULL;
    count = 0;
    rc = hyper_find_by_participant(hmem, algo1, 0, &results, &count);
    assert(rc == 0);
    int found_eval = 0;
    for (size_t i = 0; i < count; i++) {
        if (results[i].process_id == proc_make(djb2_hash("OBSERVED_OUTCOME"), PROC_KIND_EVENT))
            found_eval++;
    }
    assert(found_eval > 0);
    free(results);
    printf("%d OBSERVED_OUTCOME atoms found.\n", found_eval);

    // Фиксируем значение перед закрытием
    float saved_score = score_get(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1);
    mdb_txn_commit(txn);
    hyper_memory_free(hmem);

    // ---------- Этап 2: персистентность после переоткрытия БД ----------
    close_lmdb();
    assert(init_lmdb("./test_eval_db") == MDB_SUCCESS);
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == 0);

    hmem = hyper_memory_new(txn, atoms, idx_proc, idx_args, idx_ctx);
    assert(hmem);
    hyper_memory_set_db_causal(hmem, idx_causal);
    hyper_memory_set_db_archive(hmem, archive);
    hyper_memory_set_db_vectors(hmem, idx_vectors);

    float loaded_score = score_get(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1);
    printf("After reopen: score = %.3f (was %.3f)\n", loaded_score, saved_score);
    assert(fabsf(loaded_score - saved_score) < 1e-6f);
    assert(loaded_score > SCORE_PRIOR);

    mdb_txn_abort(txn);
    hyper_memory_free(hmem);
    close_lmdb();

    // ---------- Этап 3: score_recompute (пересчёт из истории) ----------
    // Создаём новую БД с чистого листа, чтобы история была ровно той, что мы зададим.
    system("rm -rf ./test_eval_db");
    assert(init_lmdb("./test_eval_db") == MDB_SUCCESS);
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);
    hmem = hyper_memory_new(txn, atoms, idx_proc, idx_args, idx_ctx);
    assert(hmem);
    hyper_memory_set_db_causal(hmem, idx_causal);
    hyper_memory_set_db_archive(hmem, archive);
    hyper_memory_set_db_vectors(hmem, idx_vectors);

    // Добавляем три наблюдения вручную через evaluation_record
    evaluation_record(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1, 0.8f, 0, 0);
    evaluation_record(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1, 1.0f, 0, 0);
    evaluation_record(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1, 0.6f, 0, 0);
    // Пока без Score – вызываем recompute
    rc = score_recompute(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1);
    assert(rc == 0);
    float recomputed = score_get(hmem, COGNITIVE_DOMAIN_ALGORITHM, algo1);
    // Среднее трёх наблюдений: (0.8+1.0+0.6)/3 = 0.8
    float expected = (0.8f + 1.0f + 0.6f) / 3.0f;
    printf("Recomputed score: %.3f (expected %.3f)\n", recomputed, expected);
    assert(fabsf(recomputed - expected) < 0.01f);

    mdb_txn_commit(txn);
    hyper_memory_free(hmem);
    close_lmdb();

    system("rm -rf ./test_eval_db");
    printf("Evaluation test passed.\n");
    return 0;
}
