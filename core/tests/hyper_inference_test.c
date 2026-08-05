// tests/hyper_inference_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <lmdb.h>
#include <sys/stat.h>

#include "storage/hyper_atom/hyper_atom.h"
#include "runtime/vm/vm_context.h"
#include "runtime/ops/vm_ops.h"
#include "runtime/vm/vm_status.h"

#define ID_IS_CHILD_OF 0x0001
#define ID_CAUSES      0x0100
#define ID_FIRE        0x0200
#define ID_SMOKE       0x0201
#define ID_ALARM       0x0202

int main() {
    printf("Hyper Inference Causality Test\n");
    system("rm -rf ./test_hyper_db");

    MDB_env *env;
    mdb_env_create(&env);
    mdb_env_set_maxdbs(env, 12);
    mdb_env_set_mapsize(env, 10*1024*1024);
    mkdir("./test_hyper_db", 0755);
    mdb_env_open(env, "./test_hyper_db", 0, 0664);

    MDB_txn *txn;
    mdb_txn_begin(env, NULL, 0, &txn);
    MDB_dbi dbi_atoms, dbi_idx_proc, dbi_idx_args, dbi_idx_ctx, dbi_idx_causal;
    mdb_dbi_open(txn, "atoms", MDB_CREATE, &dbi_atoms);
    mdb_dbi_open(txn, "idx_proc", MDB_CREATE | MDB_DUPSORT, &dbi_idx_proc);
    mdb_dbi_open(txn, "idx_args", MDB_CREATE | MDB_DUPSORT, &dbi_idx_args);
    mdb_dbi_open(txn, "idx_ctx",   MDB_CREATE | MDB_DUPSORT, &dbi_idx_ctx);
    mdb_dbi_open(txn, "idx_causal", MDB_CREATE | MDB_DUPSORT, &dbi_idx_causal);
    mdb_txn_commit(txn);

    mdb_txn_begin(env, NULL, 0, &txn);
    HyperMemory *hmem = hyper_memory_new(dbi_atoms, dbi_idx_proc, dbi_idx_args, dbi_idx_ctx);
    assert(hmem != NULL);
    hmem->dbi_idx_causal = dbi_idx_causal;

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.hyper_mem = hmem;
    ctx.current_context = 0;
    ctx.current_episode_id = 0x9999;
    ctx.memory.txn = txn;

    int rc;

    // ASSERT fire CAUSES smoke
    // Сигнатура: arg[0]=process_reg, arg[1]=arg0_reg, arg[2]=arg1_reg, arg[3]=dst_id_reg
    ctx.reg[1].i = ID_CAUSES;
    ctx.reg[2].i = HYPER_MAKE_REF(ID_FIRE);
    ctx.reg[3].i = HYPER_MAKE_REF(ID_SMOKE);
    Instruction i1 = {0};
    i1.arg[0]=1; i1.arg[1]=2; i1.arg[2]=3; i1.arg[3]=10;
    rc = vm_op_assert(&ctx, &i1);
    assert(rc == VM_OK);

    // ASSERT smoke CAUSES alarm
    ctx.reg[1].i = ID_CAUSES;
    ctx.reg[2].i = HYPER_MAKE_REF(ID_SMOKE);
    ctx.reg[3].i = HYPER_MAKE_REF(ID_ALARM);
    i1.arg[0]=1; i1.arg[1]=2; i1.arg[2]=3; i1.arg[3]=11;
    rc = vm_op_assert(&ctx, &i1);
    assert(rc == VM_OK);
    ko_id_t smoke_alarm_id = (ko_id_t)ctx.reg[11].i;

    // DERIVE fire CAUSES alarm с причиной smoke_alarm_id
    // Сигнатура: arg[0]=process, arg[1]=arg0, arg[2]=arg1, arg[3]=cause_reg, arg[4]=dst_id_reg
    ctx.reg[1].i = ID_CAUSES;
    ctx.reg[2].i = HYPER_MAKE_REF(ID_FIRE);
    ctx.reg[3].i = HYPER_MAKE_REF(ID_ALARM);
    ctx.reg[4].i = (int64_t)smoke_alarm_id;   // причина
    Instruction i2 = {0};
    i2.arg[0]=1; i2.arg[1]=2; i2.arg[2]=3; i2.arg[3]=4; i2.arg[4]=12;
    rc = vm_op_derive(&ctx, &i2);
    assert(rc == VM_OK);
    ko_id_t derived_id = (ko_id_t)ctx.reg[12].i;

    // TRACE от derived
    ctx.reg[12].i = (int64_t)derived_id;
    Instruction i3 = {0};
    i3.arg[0]=12; i3.arg[1]=5; i3.arg[2]=0; i3.arg[3]=14;
    rc = vm_op_trace(&ctx, &i3);
    assert(rc == VM_OK);
    uint64_t chain_len = (uint64_t)ctx.reg[14].i;
    assert(chain_len >= 2);

    ko_id_t first  = (ko_id_t)ctx.scratchpad[0].value;
    ko_id_t second = (ko_id_t)ctx.scratchpad[1].value;
    assert(first == derived_id);
    assert(second == smoke_alarm_id);

    printf("Causality trace works! derived_id=%lu, cause_id=%lu\n", derived_id, smoke_alarm_id);

    mdb_txn_commit(txn);
    hyper_memory_free(hmem);
    mdb_env_close(env);
    system("rm -rf ./test_hyper_db");
    return 0;
}
