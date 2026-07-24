// tests/critic_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "core/globals.h"
#include "storage/db/db.h"
#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/ops/opcode.h"
#include "runtime/operator/operator.h"
#include "memory/working.h"
#include "knowledge/algorithm_saver.h"

// Временные extern-объявления (пока функции карантина не вынесены в отдельный модуль)
extern bool is_quarantined(uint64_t algo_id);
extern void record_execution_result(uint64_t algo_id, int rc);

int main(void) {
    system("rm -rf ./test_critic_db");
    assert(init_lmdb("./test_critic_db") == MDB_SUCCESS);

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == MDB_SUCCESS);

    WorkingMemory wm;
    wm_init(&wm, 256, 512);
    operator_registry_init();

    // Создаём заведомо падающий алгоритм (OP_ASSERT с невалидными регистрами)
    uint64_t fail_id = 999;
    Instruction fail_code[] = {
        // Загружаем в регистр 0 значение, которое вызовет ошибку при использовании
        { .operator_id = OP_LOAD_CONST, .arg = { 0, 0 } },
        // Пытаемся выполнить несуществующий оператор — гарантированный VM_UNKNOWN_OPCODE
        { .operator_id = 9999 },
        { .operator_id = OP_HALT }
    };

    Pipeline fail_pl = { .code = fail_code, .code_len = 2, .capacity = 2,
                         .constants = {0} };
    algorithm_save(txn, fail_id, &fail_pl);
    mdb_txn_commit(txn);

    // Три запуска – все должны вернуть ошибку
    for (int i = 0; i < 3; i++) {
        MDB_txn *run_txn;
        mdb_txn_begin(db.env, NULL, 0, &run_txn);
        VMContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        vm_init(&ctx, run_txn, &wm);
        int rc = vm_execute(&ctx, &fail_pl);
        assert(rc != VM_OK);
        record_execution_result(fail_id, rc);
        vm_destroy(&ctx);
        mdb_txn_abort(run_txn);
    }

    // После трёх ошибок алгоритм должен быть в карантине
    assert(is_quarantined(fail_id) == true);

    printf("Critic quarantine test passed.\n");

    wm_clear(&wm);
    close_lmdb();
    system("rm -rf ./test_critic_db");
    return 0;
}
