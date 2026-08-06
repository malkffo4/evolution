// tests/agi_integration_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <lmdb.h>
#include <unistd.h>

#include "memory/decay.h"
#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/vm_ops.h"
#include "runtime/ops/opcode.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"

int main(void) {
    system("rm -rf ./test_agi_db");
    assert(init_lmdb("./test_agi_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    MDB_dbi atoms       = db.graph.hyper.atoms;
    MDB_dbi idx_proc    = db.graph.hyper.idx_process;
    MDB_dbi idx_args    = db.graph.hyper.idx_args;
    MDB_dbi idx_ctx     = db.graph.hyper.idx_context;
    MDB_dbi idx_causal  = db.graph.hyper.idx_causal;
    MDB_dbi archive     = db.graph.hyper.archive;
    MDB_dbi idx_vectors = db.graph.hyper.idx_vectors;

    HyperMemory *hmem = hyper_memory_new(atoms, idx_proc, idx_args, idx_ctx);
    assert(hmem != NULL);
    hyper_memory_set_db_causal(hmem, idx_causal);
    hyper_memory_set_db_archive(hmem, archive);
    hyper_memory_set_db_vectors(hmem, idx_vectors);

    ko_id_t proc_is_a = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);

    // Создаём два атома: горячий и заведомо холодный
    NeuroAtom atom_cat = {0};
    atom_cat.id = djb2_hash("cat");
    atom_cat.process_id = proc_is_a;
    atom_cat.args[0].raw = HYPER_MAKE_REF(atom_cat.id);
    atom_cat.args[1].raw = HYPER_MAKE_REF(djb2_hash("animal"));
    atom_cat.sti = 0.9f;
    atom_cat.utility = 0.5f;
    atom_cat.lti = 0.8f;
    atom_cat.truth_confidence = 0.9f;
    hyper_assert_unique(txn, hmem, &atom_cat);

    NeuroAtom atom_dog = {0};
    atom_dog.id = djb2_hash("dog");
    atom_dog.process_id = proc_is_a;
    atom_dog.args[0].raw = HYPER_MAKE_REF(atom_dog.id);
    atom_dog.args[1].raw = HYPER_MAKE_REF(djb2_hash("animal"));
    atom_dog.sti = 0.02f;          // ниже порога архивации (0.05)
    atom_dog.utility = 0.02f;      // ниже порога (0.10)
    atom_dog.lti = 0.02f;          // ниже порога (0.05)
    atom_dog.truth_confidence = 0.1f;
    hyper_assert_unique(txn, hmem, &atom_dog);

    // Сохраняем эмбеддинги
    Vector128 vec_cat = { .data = {1.0f, 0.0f, 0.5f} };
    Vector128 vec_dog = { .data = {0.9f, 0.1f, 0.4f} };
    hyper_vector_save(txn, idx_vectors, atom_cat.id, &vec_cat);
    hyper_vector_save(txn, idx_vectors, atom_dog.id, &vec_dog);
    mdb_txn_commit(txn);

    // Тест векторного поиска (read-only транзакция)
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == 0);
    VMContext ctx;
    assert(vm_init(&ctx, txn, NULL) == VM_OK);
    operator_registry_init();
    ctx.hyper_mem = hmem;

    ctx.reg[1].type = REG_INT; ctx.reg[1].i = (int64_t)atom_cat.id;
    ctx.reg[2].type = REG_FLOAT; ctx.reg[2].f = 0.5f;
    Instruction find_sim = { .operator_id = OP_FIND_SIMILAR, .arg = {1, 2, 3} };
    int rc = vm_op_find_similar(&ctx, &find_sim);
    assert(rc == VM_OK);
    assert(ctx.reg[3].node == atom_dog.id);

    // STI-фильтрация
    NeuroAtom *results = NULL;
    size_t count = 0;
    rc = hyper_find_by_process_sti(txn, hmem, proc_is_a, 0, 0, 0.5f, &results, &count);
    assert(rc == 0);
    assert(count == 1);
    assert(results[0].id == atom_cat.id);
    free(results);
    mdb_txn_abort(txn);

    // Деструктивный Decay-тест с архивацией холодного атома
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    // Диагностика: убедимся, что hmem и txn не NULL
    if (!hmem) {
        fprintf(stderr, "FATAL: hmem is NULL before decay\n");
        exit(1);
    }
    if (!txn) {
        fprintf(stderr, "FATAL: txn is NULL before decay\n");
        exit(1);
    }

    DecayStats stats;
    rc = subconscious_decay_cycle(txn, hmem, &DECAY_POLICY_DEFAULT, &stats);
    if (rc != 0) {
        fprintf(stderr, "decay_cycle failed: rc=%d, lmdb_err=%s\n", rc, mdb_strerror(rc));
        exit(1);
    }
    assert(stats.scanned > 0);
    assert(stats.archived == 1);   // dog должен уйти в архив

    // Проверяем, что dog удалён из основной таблицы
    NeuroAtom check_dog;
    MDB_val key = { sizeof(ko_id_t), &atom_dog.id };
    MDB_val val;
    int get_rc = mdb_get(txn, hmem->dbi_atoms, &key, &val);
    assert(get_rc == MDB_NOTFOUND);

    // Проверяем, что dog появился в архиве
    get_rc = mdb_get(txn, hmem->dbi_archive, &key, &val);
    assert(get_rc == MDB_SUCCESS);
    memcpy(&check_dog, val.mv_data, sizeof(NeuroAtom));
    assert(check_dog.id == atom_dog.id);
    assert(check_dog.sti < 0.2f);   // decay применён перед архивацией

    // Cat должен остаться в основной таблице с уменьшенным STI
    NeuroAtom check_cat;
    key.mv_data = &atom_cat.id;
    get_rc = mdb_get(txn, hmem->dbi_atoms, &key, &val);
    assert(get_rc == MDB_SUCCESS);
    memcpy(&check_cat, val.mv_data, sizeof(NeuroAtom));
    assert(check_cat.sti < 0.9f);

    mdb_txn_commit(txn);
    vm_destroy(&ctx);
    hyper_memory_free(hmem);
    close_lmdb();
    system("rm -rf ./test_agi_db");
    printf("AGI Integration test passed.\n");
    return 0;
}
