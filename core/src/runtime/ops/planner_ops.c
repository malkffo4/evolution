// runtime/ops/planner_ops.c
//
// "Syscall"-уровень Cognitive Cycle: три примитива оборачивают уже
// существующую и протестированную C-логику (working.c, algorithm_planner.c,
// vm_pool.c) в обычные VM-операторы. ПОЛИТИКА "что делать с целью" больше
// не хардкожена в vm_op_evaluate_goals() — она уезжает в CorePlanner
// (Pipeline в LMDB, см. subconscious.c::build_core_planner_pipeline).
// Эти функции остаются на C, потому что не являются мышлением: чтение
// ограниченной структуры (WorkingMemory, cap=100), уже вычисленный UCB1
// по Score, постановка задачи в пул акторов — ровно то разделение, что
// описано в docs/10_VM.md как Native Dispatch Table / Capability.
#include <stddef.h>

#include "core/globals.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/vm/vm_pool.h"
#include "runtime/logging/logging.h"
#include "memory/working.h"
#include "memory/subconscious.h"
#include "reasoning/planner.h"
#include "reasoning/algorithm_planner.h"
#include "knowledge/algorithm_loader.h"
#include "storage/string_pool/string_pool.h"

// OP_WM_TOP_GOAL: arg[0]=dst goal_id (REG_NODE), arg[1]=dst found (REG_INT 0/1)
// O(WM)=O(100), не O(атомов) — wm->capacity жёстко ограничена в wm_init().
int vm_op_wm_top_goal(VMContext *ctx, const Instruction *ins) {
    uint32_t r_goal  = ins->arg[0];
    uint32_t r_found = ins->arg[1];

    if (r_goal >= VM_MAX_REGISTERS || r_found >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if (!ctx->memory.wm || !ctx->hyper_mem)
        return VM_ERROR;

    node_id_t goal_id = wm_get_highest_goal(ctx->memory.wm, ctx->hyper_mem,
                                             g_homeostasis.activation_threshold);

    if (goal_id != 0) {
        ctx->reg[r_goal].type  = REG_NODE;
        ctx->reg[r_goal].node  = goal_id;
        ctx->reg[r_found].type = REG_INT;
        ctx->reg[r_found].i    = 1;
    } else {
        ctx->reg[r_goal].type  = REG_EMPTY;
        ctx->reg[r_found].type = REG_INT;
        ctx->reg[r_found].i    = 0;
    }
    return VM_OK;
}

// OP_SELECT_ALGORITHM: arg[0]=src goal_id, arg[1]=dst algo_id (REG_NODE),
// arg[2]=dst found (REG_INT 0/1).
// Внутри — planner_select_algorithm(): UCB1 по Score(ALGORITHM) через
// idx_process (не полный скан). Если кандидатов вообще нет — фиксируем
// любопытство: enqueue_research_task()+set_goal_cooldown(), тем же
// способом, каким это раньше делал захардкоженный C-фолбэк.
int vm_op_select_algorithm(VMContext *ctx, const Instruction *ins) {
    uint32_t r_goal  = ins->arg[0];
    uint32_t r_algo  = ins->arg[1];
    uint32_t r_found = ins->arg[2];

    if (r_goal >= VM_MAX_REGISTERS || r_algo >= VM_MAX_REGISTERS || r_found >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if (ctx->reg[r_goal].type != REG_NODE && ctx->reg[r_goal].type != REG_INT)
        return VM_INVALID_TYPE;
    if (!ctx->hyper_mem)
        return VM_ERROR;

    node_id_t goal_id = (ctx->reg[r_goal].type == REG_NODE)
        ? ctx->reg[r_goal].node : (node_id_t)ctx->reg[r_goal].i;

    node_id_t algo_id = 0;
    int rc = planner_select_algorithm(ctx->hyper_mem, goal_id, ctx, &algo_id);

    if (rc == 0) {
        ctx->reg[r_algo].type  = REG_NODE;
        ctx->reg[r_algo].node  = algo_id;
        ctx->reg[r_found].type = REG_INT;
        ctx->reg[r_found].i    = 1;
        return VM_OK;
    }

    const char *goal_name = get_string_from_pool(ctx->memory.txn, goal_id);
    if (goal_name)
        enqueue_research_task(goal_id, goal_name);
    set_goal_cooldown(goal_id);

    ctx->reg[r_algo].type  = REG_EMPTY;
    ctx->reg[r_found].type = REG_INT;
    ctx->reg[r_found].i    = 0;
    return VM_OK;
}

// OP_DISPATCH_ASYNC: arg[0]=src goal_id, arg[1]=src algo_id.
// Единственное место, где Instruction VM трогает потоки: грузит Pipeline
// и передаёт его в vm_pool (собственный pthread, изолированные
// HyperMemory/WorkingMemory, отдельная db_write_sync-транзакция —
// см. vm_pool.c). Вызов асинхронный: CorePlanner не ждёт результата,
// MainLoop продолжает тикать дальше.
int vm_op_dispatch_async(VMContext *ctx, const Instruction *ins) {
    uint32_t r_goal = ins->arg[0];
    uint32_t r_algo = ins->arg[1];

    if (r_goal >= VM_MAX_REGISTERS || r_algo >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if ((ctx->reg[r_goal].type != REG_NODE && ctx->reg[r_goal].type != REG_INT) ||
        (ctx->reg[r_algo].type != REG_NODE && ctx->reg[r_algo].type != REG_INT))
        return VM_INVALID_TYPE;

    node_id_t goal_id = (ctx->reg[r_goal].type == REG_NODE)
        ? ctx->reg[r_goal].node : (node_id_t)ctx->reg[r_goal].i;
    node_id_t algo_id = (ctx->reg[r_algo].type == REG_NODE)
        ? ctx->reg[r_algo].node : (node_id_t)ctx->reg[r_algo].i;

    Pipeline *algo = NULL;
    if (algorithm_load(ctx->memory.txn, algo_id, &algo) != 0 || !algo) {
        LOG_WARN("OP_DISPATCH_ASYNC: algorithm %lu not found for goal %lu",
                 (unsigned long)algo_id, (unsigned long)goal_id);
        return VM_NOT_FOUND;
    }

    set_goal_cooldown(goal_id);
    vm_pool_submit(algo, goal_id, algo_id);   /* владение algo переходит воркеру */
    return VM_OK;
}