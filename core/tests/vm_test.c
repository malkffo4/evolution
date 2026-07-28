// tests/vm_test.c
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
#include "memory/working.h"
#include "storage/db/db.h"
#include "runtime/logging/logging.h"

int main(void) {
    const char *env_path = "./test_vm_env";
    system("rm -rf ./test_vm_env");

    // Гарантируем чистое глобальное состояние базы данных
    memset(&db, 0, sizeof(db));

    int rc = init_lmdb(env_path);
    if (rc != MDB_SUCCESS) {
        fprintf(stderr, "init_lmdb failed: %s (code %d)\n", mdb_strerror(rc), rc);
        return 1;
    }

    MDB_txn *txn;
    rc = mdb_txn_begin(db.env, NULL, 0, &txn);
    assert(rc == 0);

    WorkingMemory wm_stub;
    memset(&wm_stub, 0, sizeof(wm_stub));

    VMContext ctx;
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);

    operator_registry_init();

    // Тестовая программа: R1=5, R2=10, R0=R1+R2
    Instruction code[] = {
        { .operator_id = OP_LOAD_CONST, .arg = {1, 5} },
        { .operator_id = OP_LOAD_CONST, .arg = {2, 10} },
        { .operator_id = OP_ADD,        .arg = {0, 1, 2} },
        { .operator_id = OP_HALT }
    };

    Pipeline pipeline = {0};
    pipeline.code = code;
    pipeline.code_len = 4;
    pipeline.capacity = 4;

    printf("Running VM test...\n");
    int result = vm_execute(&ctx, &pipeline);
    assert(result == VM_OK);

    int64_t r0_value = vm_register_read_int(&ctx, 0);
    assert(r0_value == 15);
    printf("VM test passed: R0 = %ld\n", r0_value);

    vm_destroy(&ctx);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_vm_env");
    return 0;
}
