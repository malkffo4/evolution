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
#include <stdlib.h>

#include "math/hash.h"
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

    if (!ctx->memory.wm) return VM_ERROR;

    node_id_t top_node = 0;
    float max_activation = 0.0f;

    // Тупой примитив: достаем самую горячую ноду из Рабочей памяти
    wm_rdlock(ctx->memory.wm);
    for (uint32_t i = 0; i < ctx->memory.wm->count; i++) {
        if (ctx->memory.wm->nodes[i].activation > max_activation) {
            max_activation = ctx->memory.wm->nodes[i].activation;
            top_node = ctx->memory.wm->nodes[i].node_id;
        }
    }
    wm_unlock(ctx->memory.wm);

    if (top_node != 0) {
        ctx->reg[r_goal].type  = REG_NODE;
        ctx->reg[r_goal].node  = top_node;
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
    uint32_t sp_base = ins->arg[1]; // Адрес в scratchpad, куда сложить ID всех алгоритмов
    uint32_t r_count = ins->arg[2]; // Регистр для записи их количества

    if (r_goal >= VM_MAX_REGISTERS || r_count >= VM_MAX_REGISTERS || sp_base >= MAX_SCRATCHPAD)
        return VM_INVALID_REGISTER;

    node_id_t goal_id = (ctx->reg[r_goal].type == REG_NODE) ? ctx->reg[r_goal].node : (node_id_t)ctx->reg[r_goal].i;

    // Ищем в графе: (goal_id) -[HAS_ALGORITHM]-> (?)
    ko_id_t has_algo_proc = proc_make(djb2_hash("HAS_ALGORITHM"), PROC_KIND_RELATION);
    NeuroAtom *results = NULL;
    size_t count = 0;
    uint32_t written = 0;

    if (hyper_find_by_process(ctx->memory.txn, ctx->hyper_mem, has_algo_proc, goal_id, 0, &results, &count) == 0) {
        for (size_t i = 0; i < count; i++) {
            // Извлекаем target (сам алгоритм)
            ko_id_t algo_id = HYPER_GET_ID(results[i].args[1].raw);
            if (sp_base + written < MAX_SCRATCHPAD) {
                ctx->scratchpad[sp_base + written].value = (int64_t)algo_id;
                written++;
            }
        }
        free(results);
    }

    ctx->reg[r_count].type = REG_INT;
    ctx->reg[r_count].i = written;

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
