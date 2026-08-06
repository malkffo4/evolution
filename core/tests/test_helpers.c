// tests/test_helpers.c
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <lmdb.h>

#include "test_helpers.h"
#include "types/id.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"

int wait_for_atom_with_args(ko_id_t participant, ko_id_t process_id, ko_id_t arg0, ko_id_t arg1, int timeout_ms) {
    const int poll_interval_ms = 50;
    int attempts = (timeout_ms + poll_interval_ms - 1) / poll_interval_ms;

    for (int a = 0; a < attempts; a++) {
        usleep(poll_interval_ms * 1000);

        MDB_txn *txn = NULL;
        if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) != MDB_SUCCESS) continue;

        HyperMemory local_hm = {0};
        local_hm.dbi_atoms = db.graph.hyper.atoms;
        local_hm.dbi_idx_process = db.graph.hyper.idx_process;
        local_hm.dbi_idx_args = db.graph.hyper.idx_args;
        local_hm.dbi_idx_context = db.graph.hyper.idx_context;

        NeuroAtom *results = NULL;
        size_t count = 0;
        int rc = hyper_find_by_participant(txn, &local_hm, participant, 0, &results, &count);
        if (rc == 0 && results && count > 0) {
            for (size_t i = 0; i < count; i++) {
                if (results[i].process_id == process_id) {
                    ko_id_t a0 = HYPER_GET_ID(results[i].args[0].raw);
                    ko_id_t a1 = HYPER_GET_ID(results[i].args[1].raw);
                    if (a0 == arg0 && a1 == arg1) {
                        free(results);
                        mdb_txn_abort(txn);
                        return 1;
                    }
                }
            }
        }
        if (results) free(results);
        mdb_txn_abort(txn);
    }
    return 0;
}
