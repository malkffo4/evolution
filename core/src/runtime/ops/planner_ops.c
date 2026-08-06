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
#include <time.h>
#include <math.h>

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

#include "storage/property/property.h"

#define GOAL_DISPATCH_COOLDOWN_SEC 10
#define GOAL_COOLDOWN_PROP_KEY "cooldown_until"

// OP_WM_TOP_GOAL: arg[0]=dst goal_id (REG_NODE), arg[1]=dst found (REG_INT 0/1)
// O(WM)=O(100), не O(атомов) — wm->capacity жёстко ограничена в wm_init().
int vm_op_wm_top_goal(VMContext *ctx, const Instruction *ins) {
    uint32_t r_goal  = ins->arg[0];
    uint32_t r_found = ins->arg[1];

    if (r_goal >= VM_MAX_REGISTERS || r_found >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if (!ctx->memory.wm || !ctx->memory.txn) return VM_ERROR;

    uint64_t now = (uint64_t)time(NULL);
    node_id_t top_node = 0;
    float max_activation = 0.0f;

    wm_rdlock(ctx->memory.wm);
    for (uint32_t i = 0; i < ctx->memory.wm->count; i++) {
        node_id_t candidate = ctx->memory.wm->nodes[i].node_id;
        float act = ctx->memory.wm->nodes[i].activation;
        if (act <= max_activation) continue;

        PropertyType pt;
        int64_t cooldown_until = 0;
        uint32_t sz = 0;
        // O(1) point lookup by (node_id, key_hash) — not a scan, safe at scale.
        if (property_get(ctx->memory.txn, candidate, GOAL_COOLDOWN_PROP_KEY,
                          &pt, &cooldown_until, sizeof(cooldown_until), &sz) == MDB_SUCCESS &&
            pt == PROP_INT && (uint64_t)cooldown_until > now) {
            continue;   // still cooling — do NOT let it win top-goal this tick
        }

        max_activation = act;
        top_node = candidate;
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
    if (!ctx->hyper_mem) return VM_ERROR;
    uint32_t r_goal  = ins->arg[0];
    uint32_t sp_base = ins->arg[1];
    uint32_t r_count = ins->arg[2];

    if (r_goal >= VM_MAX_REGISTERS || r_count >= VM_MAX_REGISTERS || sp_base >= MAX_SCRATCHPAD)
        return VM_INVALID_REGISTER;

    node_id_t goal_id = (ctx->reg[r_goal].type == REG_NODE) ? ctx->reg[r_goal].node : (node_id_t)ctx->reg[r_goal].i;

    ko_id_t has_algo_proc = proc_make(djb2_hash("HAS_ALGORITHM"), PROC_KIND_RELATION);
    NeuroAtom *results = NULL;
    size_t count = 0;

    node_id_t best_algo = 0;
    float best_ucb = -1.0f;
    float exploration_param = 0.5f; // UCB1 параметр (любопытство)

    if (hyper_find_by_process(ctx->memory.txn, ctx->hyper_mem, has_algo_proc, goal_id, 0, &results, &count) == 0) {
        for (size_t i = 0; i < count; i++) {
            ko_id_t arg0 = HYPER_GET_ID(results[i].args[0].raw);
            ko_id_t arg1 = HYPER_GET_ID(results[i].args[1].raw);
            ko_id_t algo_id = (arg0 == HYPER_GET_ID(goal_id)) ? arg1 : arg0;

            if (algo_id == HYPER_GET_ID(goal_id)) continue;

            float mean = 0.5f;
            float confidence = 0.01f;

            // Ищем HAS_SCORE для данного алгоритма, чтобы оценить его успешность
            ko_id_t has_score_proc = proc_make(djb2_hash("HAS_SCORE"), PROC_KIND_RELATION);
            NeuroAtom *score_atoms = NULL;
            size_t score_count = 0;

            if (hyper_find_by_participant(ctx->memory.txn, ctx->hyper_mem, algo_id, 0, &score_atoms, &score_count) == 0) {
                for (size_t j = 0; j < score_count; j++) {
                    if (score_atoms[j].process_id == has_score_proc) {
                        mean = score_atoms[j].truth_mean;
                        confidence = score_atoms[j].truth_confidence;
                        break;
                    }
                }
                free(score_atoms);
            }

            // Формула UCB1: Базовая успешность + бонус за неизведанность
            float ucb = mean + exploration_param * sqrtf(1.0f / (confidence + 0.001f));

            if (ucb > best_ucb) {
                best_ucb = ucb;
                best_algo = algo_id;
            }
        }
        free(results);
    }

    if (best_algo != 0) {
        ctx->scratchpad[sp_base].value = (int64_t)best_algo;
        ctx->reg[r_count].type = REG_INT;
        ctx->reg[r_count].i = 1;
    } else {
        ctx->reg[r_count].type = REG_INT;
        ctx->reg[r_count].i = 0;
    }

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

    int64_t cooldown_until = (int64_t)time(NULL) + GOAL_DISPATCH_COOLDOWN_SEC;
    property_set(ctx->memory.txn, goal_id, GOAL_COOLDOWN_PROP_KEY,
                 PROP_INT, &cooldown_until, sizeof(cooldown_until));

    wm_wrlock(ctx->memory.wm);
    for (uint32_t i = 0; i < ctx->memory.wm->count; i++) {
        if (ctx->memory.wm->nodes[i].node_id == goal_id) {
            ctx->memory.wm->nodes[i].activation *= 0.4f;   // dispatched -> no longer "urgent"
            break;
        }
    }
    wm_unlock(ctx->memory.wm);

    vm_pool_submit(algo, goal_id, algo_id);
    return VM_OK;
}
