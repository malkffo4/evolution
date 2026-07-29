// tests/trust_test.c
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
#include "knowledge/trust.h"          // наш модуль

int main(void) {
    system("rm -rf ./test_trust_db");
    assert(init_lmdb("./test_trust_db") == MDB_SUCCESS);

    // ---------- Этап 1: обновление доверия и проверка in-memory ----------
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

    // 1. Начальное доверие – отсутствует, должно быть TRUST_PRIOR
    float t1 = trust_get(hmem, TRUST_DOMAIN_ALGORITHM, algo1);
    assert(t1 == TRUST_PRIOR);

    // 2. Успешные запуски: доверие растёт
    for (int i = 0; i < 5; i++) {
        int rc = trust_update(hmem, TRUST_DOMAIN_ALGORITHM, algo1, true);
        assert(rc == 0);
    }
    t1 = trust_get(hmem, TRUST_DOMAIN_ALGORITHM, algo1);
    assert(t1 > TRUST_PRIOR);                       // стало больше начального
    printf("After 5 successes: trust = %.3f\n", t1);

    // 3. Неудача – доверие падает
    trust_update(hmem, TRUST_DOMAIN_ALGORITHM, algo1, false);
    float t1_after_fail = trust_get(hmem, TRUST_DOMAIN_ALGORITHM, algo1);
    assert(t1_after_fail < t1);                     // уменьшилось
    printf("After 1 failure: trust = %.3f\n", t1_after_fail);

    // 4. При равном доверии pick_best не ломается (просто проверим, что trust_get
    //    возвращает одинаковые значения для двух субъектов без ошибок)
    trust_update(hmem, TRUST_DOMAIN_ALGORITHM, algo2, true);  // одно успешное обновление
    float t2 = trust_get(hmem, TRUST_DOMAIN_ALGORITHM, algo2);
    // algo2 тоже должен иметь доверие выше TRUST_PRIOR, но может отличаться
    printf("Trust for algo2: %.3f\n", t2);
    // pick_best сравнивает два float – проблем с равенством/NaN не предвидится
    // Дополнительно удостоверимся, что оба значения конечны
    assert(isfinite(t1_after_fail));
    assert(isfinite(t2));

    // 5. Проверка, что hyper_find_by_participant(algo1) возвращает атом TRUST_SCORE
    NeuroAtom *results = NULL;
    size_t count = 0;
    int rc = hyper_find_by_participant(hmem, algo1, 0, &results, &count);
    assert(rc == 0);
    int found_trust = 0;
    for (size_t i = 0; i < count; i++) {
        if (results[i].process_id == djb2_hash("TRUST_SCORE")) {
            found_trust++;
            // Проверяем структуру: args[0] == algo1, args[1] == домен (int)
            assert(HYPER_GET_ID(results[i].args[0].raw) == algo1);
            assert((results[i].args[1].raw & HYPER_TYPE_MASK) == HYPER_TYPE_INT);
        }
    }
    assert(found_trust == 1);
    free(results);
    printf("TRUST_SCORE atom found via participant index.\n");

    // Фиксируем значение для последующей проверки персистентности
    float saved_trust = trust_get(hmem, TRUST_DOMAIN_ALGORITHM, algo1);
    mdb_txn_commit(txn);
    hyper_memory_free(hmem);

    // ---------- Этап 2: переоткрытие БД и проверка сохранения ----------
    close_lmdb();                       // закрываем среду
    // Инициализируем заново (та же директория, данные остались)
    assert(init_lmdb("./test_trust_db") == MDB_SUCCESS);
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == 0);

    // Создаём новый HyperMemory с read-only транзакцией
    hmem = hyper_memory_new(txn, atoms, idx_proc, idx_args, idx_ctx);
    assert(hmem);
    hyper_memory_set_db_causal(hmem, idx_causal);
    hyper_memory_set_db_archive(hmem, archive);
    hyper_memory_set_db_vectors(hmem, idx_vectors);

    // 6. Проверяем, что атом доверия сохранился между перезапусками
    float loaded_trust = trust_get(hmem, TRUST_DOMAIN_ALGORITHM, algo1);
    printf("After reopen: trust = %.3f (was %.3f)\n", loaded_trust, saved_trust);
    assert(loaded_trust == saved_trust);   // точное соответствие
    assert(loaded_trust > TRUST_PRIOR);    // не сбросилось

    mdb_txn_abort(txn);
    hyper_memory_free(hmem);
    close_lmdb();

    system("rm -rf ./test_trust_db");
    printf("Trust test passed.\n");
    return 0;
}
