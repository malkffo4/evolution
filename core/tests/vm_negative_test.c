// core/tests/vm_negative_test.c
// Negative-тесты VM: проверяют, что произвольно повреждённый/некорректный
// байткод НИКОГДА не приводит к падению процесса (segfault/UB), а всегда
// возвращает контролируемый VMStatus-код ошибки.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_param.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "runtime/ops/vm_ops.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "knowledge/algorithm_saver.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); tests_passed++; } \
    else { printf("[FAIL] %s\n", msg); tests_failed++; } \
} while (0)

static VMContext *make_ctx(MDB_txn *txn, WorkingMemory *wm) {
    VMContext *ctx = malloc(sizeof(VMContext));
    assert(ctx != NULL);
    assert(vm_init(ctx, txn, wm) == VM_OK);
    ctx->hyper_mem = hyper_memory_new(db.graph.hyper.atoms, db.graph.hyper.idx_process, db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    return ctx;
}

int main(void) {
    system("rm -rf ./test_vm_negative_db");
    assert(init_lmdb("./test_vm_negative_db") == MDB_SUCCESS);
    operator_registry_init();

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);
    WorkingMemory wm;
    memset(&wm, 0, sizeof(wm));

    /* ---- 1. OOB регистр на OP_BRANCH_IF_EMPTY (реально найденный баг) ---- */
    {
        VMContext *ctx = make_ctx(txn, &wm);
        Instruction code[] = {
            { .operator_id = OP_BRANCH_IF_EMPTY, .arg = { 999999, 0 } },
            { .operator_id = OP_HALT }
        };
        Pipeline p = { .code = code, .code_len = 2, .capacity = 2 };
        int rc = vm_execute(ctx, &p);
        CHECK(rc == VM_INVALID_REGISTER,
              "OP_BRANCH_IF_EMPTY с OOB-регистром возвращает VM_INVALID_REGISTER, не крашится");
        hyper_memory_free(ctx->hyper_mem);
        vm_destroy(ctx); free(ctx);
    }

    /* ---- 2. Неизвестный опкод ---- */
    {
        VMContext *ctx = make_ctx(txn, &wm);
        Instruction code[] = {
            { .operator_id = 999999 },
            { .operator_id = OP_HALT }
        };
        Pipeline p = { .code = code, .code_len = 2, .capacity = 2 };
        int rc = vm_execute(ctx, &p);
        CHECK(rc == VM_UNKNOWN_OPCODE, "Неизвестный opcode возвращает VM_UNKNOWN_OPCODE");
        hyper_memory_free(ctx->hyper_mem);
        vm_destroy(ctx); free(ctx);
    }

    /* ---- 3. Деление на ноль ---- */
    {
        VMContext *ctx = make_ctx(txn, &wm);
        Instruction code[] = {
            { .operator_id = OP_LOAD_CONST, .arg = {1, 42} },
            { .operator_id = OP_LOAD_CONST, .arg = {2, 0} },
            { .operator_id = OP_DIV, .arg = {0, 1, 2} },
            { .operator_id = OP_HALT }
        };
        Pipeline p = { .code = code, .code_len = 4, .capacity = 4 };
        int rc = vm_execute(ctx, &p);
        CHECK(rc == VM_ERROR, "Деление на ноль -> VM_ERROR, а не SIGFPE");
        hyper_memory_free(ctx->hyper_mem);
        vm_destroy(ctx); free(ctx);
    }

    /* ---- 4. OOB регистры на арифметике ---- */
    {
        VMContext *ctx = make_ctx(txn, &wm);
        Instruction code[] = { { .operator_id = OP_ADD, .arg = {0, 70, 71} }, { .operator_id = OP_HALT } };
        Pipeline p = { .code = code, .code_len = 2, .capacity = 2 };
        int rc = vm_execute(ctx, &p);
        CHECK(rc == VM_INVALID_REGISTER, "OP_ADD с OOB операндами возвращает VM_INVALID_REGISTER");
        hyper_memory_free(ctx->hyper_mem);
        vm_destroy(ctx); free(ctx);
    }

    /* ---- 5. OOB регистр на OP_MOVE ---- */
    {
        VMContext *ctx = make_ctx(txn, &wm);
        Instruction code[] = { { .operator_id = OP_MOVE, .arg = {5000, 0} }, { .operator_id = OP_HALT } };
        Pipeline p = { .code = code, .code_len = 2, .capacity = 2 };
        int rc = vm_execute(ctx, &p);
        CHECK(rc == VM_INVALID_REGISTER, "OP_MOVE с OOB dst регистром возвращает VM_INVALID_REGISTER");
        hyper_memory_free(ctx->hyper_mem);
        vm_destroy(ctx); free(ctx);
    }

    /* ---- 6. OOB регистр на OP_CHECK_CACHED_EDGE ---- */
    {
        VMContext *ctx = make_ctx(txn, &wm);
        Instruction code[] = { { .operator_id = OP_CHECK_CACHED_EDGE, .arg = {0, 65535, 2, 3} }, { .operator_id = OP_HALT } };
        Pipeline p = { .code = code, .code_len = 2, .capacity = 2 };
        int rc = vm_execute(ctx, &p);
        CHECK(rc == VM_INVALID_REGISTER, "OP_CHECK_CACHED_EDGE с OOB регистром возвращает VM_INVALID_REGISTER");
        hyper_memory_free(ctx->hyper_mem);
        vm_destroy(ctx); free(ctx);
    }

    /* ---- 7. Защита от переполнения стека через самовызывающийся алгоритм ---- */
    {
        uint64_t algo_id = 0xDEADBEEFULL;
        Instruction recursive_code[] = {
            { .operator_id = OP_LOAD_CONST, .arg = {5, 0} },
            { .operator_id = OP_EXEC_ALGORITHM, .arg = {5} },
            { .operator_id = OP_HALT }
        };
        Pipeline recursive_pl = { .code = recursive_code, .code_len = 3, .capacity = 3 };
        recursive_pl.constants.int_consts = malloc(sizeof(int64_t));
        recursive_pl.constants.int_consts[0] = (int64_t)algo_id;
        recursive_pl.constants.int_count = 1;
        assert(algorithm_save(txn, algo_id, &recursive_pl) == MDB_SUCCESS);
        free(recursive_pl.constants.int_consts);

        VMContext *ctx = make_ctx(txn, &wm);
        Instruction call_code[] = {
            { .operator_id = OP_LOAD_CONST, .arg = {5, 0} },
            { .operator_id = OP_EXEC_ALGORITHM, .arg = {5} },
            { .operator_id = OP_HALT }
        };
        Pipeline call_pl = { .code = call_code, .code_len = 3, .capacity = 3 };
        call_pl.constants.int_consts = malloc(sizeof(int64_t));
        call_pl.constants.int_consts[0] = (int64_t)algo_id;
        call_pl.constants.int_count = 1;

        int rc = vm_execute(ctx, &call_pl);
        CHECK(rc == VM_STACK_OVERFLOW,
              "Самовызывающийся exec_algorithm упирается в VM_STACK_OVERFLOW, а не в реальный краш стека");
        hyper_memory_free(ctx->hyper_mem);
        free(call_pl.constants.int_consts);
        vm_destroy(ctx); free(ctx);
    }

    /* ---- 8. OOB регистр в графовом OP_ASSERT (напрямую, не через eval_graph) ---- */
    {
        VMContext *ctx = make_ctx(txn, &wm);
        Instruction code[] = { { .operator_id = OP_ASSERT, .arg = {0, 1, 2, 99999} }, { .operator_id = OP_HALT } };
        Pipeline p = { .code = code, .code_len = 2, .capacity = 2 };
        int rc = vm_execute(ctx, &p);
        CHECK(rc == VM_INVALID_REGISTER, "OP_ASSERT с OOB dst регистром возвращает VM_INVALID_REGISTER");
        hyper_memory_free(ctx->hyper_mem);
        vm_destroy(ctx); free(ctx);
    }

    printf("\n=== VM Negative Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_vm_negative_db");
    return tests_failed ? 1 : 0;
}
