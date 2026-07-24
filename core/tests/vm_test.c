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

// Заглушки глобальных переменных для линковки с остальными модулями
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
    // 1. Создаём in-memory LMDB окружение (с временной директорией)
    const char *env_path = "./test_vm_env";
    MDB_env *env;
    mdb_env_create(&env);
    mdb_env_set_maxdbs(env, 1);
    assert(mdb_env_open(env, env_path, MDB_NOSUBDIR | MDB_NOMETASYNC, 0664) == 0);

    MDB_txn *txn;
    assert(mdb_txn_begin(env, NULL, 0, &txn) == 0);

    // 2. Инициализируем VMContext
    init_working_memory_stub(&wm_stub);
    VMContext ctx;
    assert(vm_init(&ctx, txn, &wm_stub) == VM_OK);

    // 3. Регистрируем операторы
    register_test_operators();

    // 4. Строим программу (Pipeline) с константами
    Pipeline pipeline = {0};
    pipeline.constants.int_consts = malloc(2 * sizeof(int64_t));
    pipeline.constants.int_consts[0] = 5;
    pipeline.constants.int_consts[1] = 10;
    pipeline.constants.int_count = 2;

    Instruction load_r1 = {
        .operator_id = OP_LOAD_CONST,
        .arg[0] = 1,  // R1
        .arg[1] = 0   // const[0] = 5
    };
    Instruction load_r2 = {
        .operator_id = OP_LOAD_CONST,
        .arg[0] = 2,  // R2
        .arg[1] = 1   // const[1] = 10
    };
    Instruction add = {
        .operator_id = OP_ADD,
        .arg[0] = 0,  // R0 = R1 + R2
        .arg[1] = 1,
        .arg[2] = 2
    };
    Instruction halt = { .operator_id = OP_HALT };

    Instruction prog[] = { load_r1, load_r2, add, halt };
    pipeline.code = prog;
    pipeline.code_len = sizeof(prog) / sizeof(Instruction);

    // 5. Запуск VM
    printf("Running VM test...\n");
    int result = vm_execute(&ctx, &pipeline);
    assert(result == VM_OK);

    // 6. Проверяем результат
    int64_t r0_value = vm_register_read_int(&ctx, 0);
    assert(r0_value == 15);
    printf("VM test passed: R0 = %ld\n", r0_value);

    // 7. Очистка
    free(pipeline.constants.int_consts);
    vm_destroy(&ctx);
    mdb_txn_abort(txn);
    mdb_env_close(env);
    // Удаляем временную директорию
    system("rm -rf ./test_vm_env");
    system("rm -f test_vm_env-lock");
    return 0;
}
