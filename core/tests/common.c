// tests/common.c
#include <lmdb.h>

#include "math/hash.h"
#include "runtime/vm/instruction.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/ops/opcode.h"
#include "knowledge/algorithm_saver.h"

int planner_bootstrap(MDB_txn *txn) {
    // Создаем декларативный CorePlanner на лету для теста
    Instruction core_planner_code[] = {
        /*0*/ { .operator_id = OP_WM_TOP_GOAL,      .arg = { 1, 2 } },
        /*1*/ { .operator_id = OP_BRANCH_IF_EMPTY,  .arg = { 1, 5 } }, // ПРОВЕРЯЕМ r1! Прыгаем на HALT если пусто
        /*2*/ { .operator_id = OP_SELECT_ALGORITHM, .arg = { 1, 0, 3 } },
        /*3*/ { .operator_id = OP_READ_SP,          .arg = { 4, 0 } }, // ЧИТАЕМ память (sp[0]) в регистр 4!
        /*4*/ { .operator_id = OP_DISPATCH_ASYNC,   .arg = { 1, 4 } }, // ПЕРЕДАЕМ регистр 4!
        /*5*/ { .operator_id = OP_HALT }
    };
    Pipeline core_planner_pipeline = {
        .code = core_planner_code,
        .code_len = 6, // ТУТ ТЕПЕРЬ 6 ИНСТРУКЦИЙ!
        .capacity = 6
    };
    int rc = algorithm_save(txn, djb2_hash("CorePlanner"), &core_planner_pipeline);
}
