// tests/arithmetic_ops_test.c
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "runtime/ops/vm_ops.h"
#include "runtime/register/register.h"
#include "memory/working.h"
#include "storage/db/db.h"

int main(void) {
    system("rm -rf ./test_arith_db");
    assert(init_lmdb("./test_arith_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);
    WorkingMemory wm_stub = {0};
    VMContext ctx;
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);
    operator_registry_init();

    // (120000 + 90000 + 60000) / 3 = 90000 -- "average salary"
    Instruction code[] = {
        { .operator_id = OP_LOAD_CONST, .arg = {1, 120000} },
        { .operator_id = OP_LOAD_CONST, .arg = {2, 90000} },
        { .operator_id = OP_ADD,        .arg = {3, 1, 2} },
        { .operator_id = OP_LOAD_CONST, .arg = {4, 60000} },
        { .operator_id = OP_ADD,        .arg = {3, 3, 4} },
        { .operator_id = OP_LOAD_CONST, .arg = {5, 3} },
        { .operator_id = OP_DIV,        .arg = {0, 3, 5} },
        { .operator_id = OP_HALT }
    };
    Pipeline pipeline = { .code = code, .code_len = 8, .capacity = 8 };
    assert(vm_execute(&ctx, &pipeline) == VM_OK);
    assert(vm_register_read_int(&ctx, 0) == 90000);
    printf("average(120000,90000,60000) = %ld\n", (long)vm_register_read_int(&ctx, 0));

    ctx.reg[1].type = REG_INT; ctx.reg[1].i = 50;
    ctx.reg[2].type = REG_INT; ctx.reg[2].i = 8;
    Instruction sub_ins = { .operator_id = OP_SUB, .arg = {0, 1, 2} };
    assert(vm_op_sub(&ctx, &sub_ins) == VM_OK && ctx.reg[0].i == 42);

    Instruction mul_ins = { .operator_id = OP_MUL, .arg = {0, 1, 2} };
    assert(vm_op_mul(&ctx, &mul_ins) == VM_OK && ctx.reg[0].i == 400);

    ctx.reg[2].i = 0;
    Instruction div_zero = { .operator_id = OP_DIV, .arg = {0, 1, 2} };
    assert(vm_op_div(&ctx, &div_zero) == VM_ERROR);

    Instruction bad_type = { .operator_id = OP_SUB, .arg = {0, 63, 1} }; // reg 63 = REG_EMPTY
    assert(vm_op_sub(&ctx, &bad_type) == VM_INVALID_TYPE);

    printf("Arithmetic ops test passed (SUB/MUL/DIV).\n");
    vm_destroy(&ctx);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_arith_db");
    return 0;
}
