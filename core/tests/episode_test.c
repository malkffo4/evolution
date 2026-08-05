// tests/episode_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <lmdb.h>

#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "knowledge/episode.h"
#include "runtime/vm/vm_status.h"
#include "math/hash.h"

int main(void) {
    system("rm -rf ./test_episode_db");
    assert(init_lmdb("./test_episode_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    HyperMemory *hmem = hyper_memory_new(db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    assert(hmem != NULL);

    node_id_t GOAL = djb2_hash("SortArray");
    node_id_t ALGO = djb2_hash("QuickSortAlgo");

    // ДВА эпизода с ОДИНАКОВЫМ (goal, algo), разным исходом. Проверяет,
    // что episode_record() использует hyper_assert() (безусловно), а НЕ
    // hyper_assert_unique() — иначе второй указатель-атом был бы молча
    // отброшен как "дубликат" по (process_id,args), несмотря на разные id.
    Episode ep1 = {0};
    ep1.id = hyper_memory_new_id(hmem);
    ep1.goal_id = GOAL; ep1.algorithm_id = ALGO;
    ep1.vm_status = VM_ERROR; ep1.outcome = 0.0f;
    ep1.start_cycles = 1000; ep1.duration_cycles = 50; ep1.wall_time = 111111;
    assert(episode_record(txn, hmem, &ep1) == 0);

    Episode ep2 = {0};
    ep2.id = hyper_memory_new_id(hmem);
    assert(ep2.id != ep1.id);
    ep2.goal_id = GOAL; ep2.algorithm_id = ALGO; ep2.result_atom_id = 777;
    ep2.vm_status = VM_OK; ep2.outcome = 1.0f;
    ep2.start_cycles = 2000; ep2.duration_cycles = 30; ep2.wall_time = 222222;
    assert(episode_record(txn, hmem, &ep2) == 0);

    Episode loaded1, loaded2;
    assert(episode_load(txn, ep1.id, &loaded1) == MDB_SUCCESS);
    assert(episode_load(txn, ep2.id, &loaded2) == MDB_SUCCESS);
    assert(loaded1.vm_status == VM_ERROR && loaded1.outcome == 0.0f && loaded1.duration_cycles == 50);
    assert(loaded2.result_atom_id == 777 && loaded2.vm_status == VM_OK && loaded2.outcome == 1.0f);

    Episode missing;
    assert(episode_load(txn, 0xDEADBEEF, &missing) == MDB_NOTFOUND);

    NeuroAtom *by_goal = NULL;
    size_t goal_count = 0;
    assert(hyper_find_by_participant(txn, hmem, GOAL, 0, &by_goal, &goal_count) == 0);
    int found1 = 0, found2 = 0;
    node_id_t episode_proc = proc_make(djb2_hash("EPISODE_RECORDED"), PROC_KIND_EVENT);
    for (size_t i = 0; i < goal_count; i++) {
        if (by_goal[i].process_id != episode_proc) continue;
        if (by_goal[i].id == ep1.id) found1 = 1;
        if (by_goal[i].id == ep2.id) found2 = 1;
    }
    assert(found1 && found2);
    free(by_goal);

    NeuroAtom *by_algo = NULL;
    size_t algo_count = 0;
    assert(hyper_find_by_participant(txn, hmem, ALGO, 0, &by_algo, &algo_count) == 0);
    int found_by_algo = 0;
    for (size_t i = 0; i < algo_count; i++)
        if (by_algo[i].process_id == episode_proc && by_algo[i].id == ep2.id) found_by_algo = 1;
    assert(found_by_algo);
    free(by_algo);

    // --- Персистентность: переоткрываем LMDB (тот же приём, что в
    // evaluation_test.c "Этап 2") и проверяем, что Episode пережил "рестарт".
    mdb_txn_commit(txn);
    hyper_memory_free(hmem);
    close_lmdb();

    assert(init_lmdb("./test_episode_db") == MDB_SUCCESS);
    MDB_txn *txn2;
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn2) == 0);
    Episode reloaded;
    assert(episode_load(txn2, ep2.id, &reloaded) == MDB_SUCCESS);
    assert(reloaded.outcome == 1.0f && reloaded.result_atom_id == 777);
    mdb_txn_abort(txn2);

    close_lmdb();
    system("rm -rf ./test_episode_db");
    printf("Episode test passed: record/load, dual-participant discovery, restart persistence.\n");
    return 0;
}
