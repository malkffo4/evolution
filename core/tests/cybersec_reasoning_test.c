#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_pool.h"
#include "runtime/vm/instruction.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/edge/edge.h"
#include "storage/graph/graph.h"
#include "storage/string_pool/string_pool.h"
#include "math/hash.h"
#include "knowledge/knowledge_cache.h"
#include "knowledge/algorithm_saver.h"

int main(void) {
    system("rm -rf ./test_cybersec_db");
    assert(init_lmdb("./test_cybersec_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    // Initial setup
    uint64_t algo_id = djb2_hash("CybersecAnalyzeAlgo");
    Instruction algo_code[] = {
        { .operator_id = OP_HALT } // simple pipeline just halts
    };

    Pipeline* algo_pipeline = pipeline_create_with_capacity(1);
    algo_pipeline->code[0] = algo_code[0];
    algo_pipeline->code_len = 1;
    algo_pipeline->constants.int_consts = NULL;
    algo_pipeline->constants.int_count = 0;

    assert(algorithm_save(txn, algo_id, algo_pipeline) == MDB_SUCCESS);
    mdb_txn_commit(txn);

    operator_registry_init();

    // HyperMemory dummy
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == 0);
    HyperMemory *hm = hyper_memory_new(txn,
                                     db.graph.hyper.atoms,
                                     db.graph.hyper.idx_process,
                                     db.graph.hyper.idx_args,
                                     db.graph.hyper.idx_context);

    // Call asynchronous pool execute
    // Create new pipeline object since vm_pool_submit will take ownership of it
    Pipeline* exec_pipe = pipeline_create_with_capacity(1);
    exec_pipe->code[0] = algo_code[0];
    exec_pipe->code_len = 1;

    WorkingMemory wm_stub;
    assert(wm_init(&wm_stub, 256, 512) == 0);

    vm_pool_submit(exec_pipe, hm, &wm_stub);

    // Wait slightly to let the background thread finish
    usleep(100000);

    printf("Cybersec asynchronous integration test passed: vm_pool_submit completed without blocking.\n");

    hyper_memory_free(hm);
    wm_clear(&wm_stub);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_cybersec_db");

    pipeline_free(algo_pipeline);
    return 0;
}
