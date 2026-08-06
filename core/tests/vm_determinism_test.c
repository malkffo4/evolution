// core/tests/vm_determinism_test.c
// Determinism-тест: доказывает, что один и тот же байткод, запущенный
// трижды с нуля (свежая LMDB каждый раз), даёт БИТ-В-БИТ идентичный
// результат — и в чистой арифметике, и в порождённом графовом знании
// (process/args/truth). Это фундаментальное требование для
// воспроизводимости AGI-решений (docs/EVENT_MODEL.md, docs/API.md).
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
#include "runtime/register/register.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"

typedef struct {
    int64_t  r0;
    float    truth_mean;
    float    truth_confidence;
    ko_id_t  process_id;
    ko_id_t  arg0;
    ko_id_t  arg1;
} RunResult;

static void run_fixed_pipeline(const char *db_path, RunResult *out) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", db_path);
    system(cmd);

    assert(init_lmdb(db_path) == MDB_SUCCESS);
    operator_registry_init();

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    HyperMemory *hmem = hyper_memory_new(db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    assert(hmem != NULL);

    WorkingMemory wm;
    memset(&wm, 0, sizeof(wm));
    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    assert(vm_init(&ctx, txn, &wm) == VM_OK);
    ctx.hyper_mem = hmem;

    node_id_t A   = djb2_hash("DeterminismA");
    node_id_t B   = djb2_hash("DeterminismB");
    node_id_t REL = djb2_hash("DETERMINISTIC_RELATION");

    Instruction code[] = {
        { .operator_id = OP_LOAD_CONST, .arg = {1, 7} },
        { .operator_id = OP_LOAD_CONST, .arg = {2, 6} },
        { .operator_id = OP_MUL,        .arg = {0, 1, 2} },   /* r0 = 42 */
        { .operator_id = OP_HALT }
    };
    Pipeline p = { .code = code, .code_len = 4, .capacity = 4 };
    assert(vm_execute(&ctx, &p) == VM_OK);
    out->r0 = ctx.reg[0].i;

    ctx.reg[10].type = REG_INT; ctx.reg[10].i = (int64_t)REL;
    ctx.reg[11].type = REG_INT; ctx.reg[11].i = (int64_t)HYPER_MAKE_REF(A);
    ctx.reg[12].type = REG_INT; ctx.reg[12].i = (int64_t)HYPER_MAKE_REF(B);

    Instruction assert_code[] = {
        { .operator_id = OP_ASSERT, .arg = {10, 11, 12, 14} }
    };
    Pipeline p2 = { .code = assert_code, .code_len = 1, .capacity = 1 };
    assert(vm_execute(&ctx, &p2) == VM_OK);

    node_id_t asserted_id = (node_id_t)ctx.reg[14].i;
    NeuroAtom atom;
    MDB_val key = { sizeof(node_id_t), &asserted_id };
    MDB_val data;
    assert(mdb_get(txn, hmem->dbi_atoms, &key, &data) == MDB_SUCCESS);
    memcpy(&atom, data.mv_data, sizeof(NeuroAtom));

    out->truth_mean       = atom.truth_mean;
    out->truth_confidence = atom.truth_confidence;
    out->process_id       = atom.process_id;
    out->arg0             = HYPER_GET_ID(atom.args[0].raw);
    out->arg1             = HYPER_GET_ID(atom.args[1].raw);

    vm_destroy(&ctx);
    hyper_memory_free(hmem);
    mdb_txn_abort(txn);
    close_lmdb();
    system(cmd);
}

int main(void) {
    RunResult run1, run2, run3;

    run_fixed_pipeline("./test_det_db_run1", &run1);
    run_fixed_pipeline("./test_det_db_run2", &run2);
    run_fixed_pipeline("./test_det_db_run3", &run3);

    int ok = 1;

    if (run1.r0 != 42 || run2.r0 != 42 || run3.r0 != 42) {
        printf("[FAIL] арифметический результат не детерминирован/неверен: %ld %ld %ld\n",
               (long)run1.r0, (long)run2.r0, (long)run3.r0);
        ok = 0;
    } else {
        printf("[PASS] арифметика детерминирована в 3 независимых запусках (r0=42)\n");
    }

    if (memcmp(&run1, &run2, sizeof(RunResult)) != 0 ||
        memcmp(&run2, &run3, sizeof(RunResult)) != 0) {
        printf("[FAIL] порождённый атом знания различается между идентичными запусками\n");
        ok = 0;
    } else {
        printf("[PASS] атом (process/args/truth) бит-в-бит идентичен в 3 запусках\n");
        printf("       process_id=%lu arg0=%lu arg1=%lu truth_mean=%.3f truth_confidence=%.3f\n",
               (unsigned long)run1.process_id, (unsigned long)run1.arg0, (unsigned long)run1.arg1,
               run1.truth_mean, run1.truth_confidence);
    }

    if (ok) {
        printf("\n[OK] Исполнительное ядро NeuroCore детерминировано.\n");
        return 0;
    }
    return 1;
}
