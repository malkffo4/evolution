// tests/vm_test.c
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <assert.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/instruction.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "memory/working.h"
#include "runtime/ops/vm_ops.h"

WorkingMemory global_wm;
volatile sig_atomic_t g_running = 1;

static WorkingMemory wm_stub;
static void init_working_memory_stub(WorkingMemory *wm) {
    (void)wm;
}

static void register_test_operators(void) {
    operator_registry_init();
}

int main(void) {
    const char *env_path = "./test_vm_env";
    MDB_env *env;
    mdb_env_create(&env);
    mdb_env_set_maxdbs(env, 1);
    assert(mdb_env_open(env, env_path, MDB_NOSUBDIR | MDB_NOMETASYNC, 0664) == 0);

    MDB_txn *txn;
    assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);

    init_working_memory_stub(&wm_stub);
    VMContext ctx;
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);

    register_test_operators();

    // Строим программу с непосредственными значениями (новый load_const)
    Instruction code[] = {
        { .operator_id = OP_LOAD_CONST, .arg[0] = 1, .arg[1] = 5 },   // R1 = 5
        { .operator_id = OP_LOAD_CONST, .arg[0] = 2, .arg[1] = 10 },  // R2 = 10
        { .operator_id = OP_ADD,        .arg[0] = 0, .arg[1] = 1, .arg[2] = 2 }, // R0 = R1 + R2
        { .operator_id = OP_HALT }
    };

    Pipeline pipeline = {0};
    pipeline.code = code;
    pipeline.code_len = 4;
    pipeline.capacity = 4;
    // Константный пул не используется
    pipeline.constants.int_consts = NULL;
    pipeline.constants.int_count = 0;

    printf("Running VM test...\n");
    int result = vm_execute(&ctx, &pipeline);
    assert(result == VM_OK);

    int64_t r0_value = vm_register_read_int(&ctx, 0);
    assert(r0_value == 15);
    printf("VM test passed: R0 = %ld\n", r0_value);

    vm_destroy(&ctx);
    mdb_txn_abort(txn);
    mdb_env_close(env);
    system("rm -rf ./test_vm_env");
    system("rm -f test_vm_env-lock");
    return 0;
}
