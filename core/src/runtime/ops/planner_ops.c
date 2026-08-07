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
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <lmdb.h>

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

// ФИКС ROOT-CAUSE #2: раньше OP_WM_TOP_GOAL выбирал АБСОЛЮТНО ЛЮБОЙ узел
// WorkingMemory с наибольшей активацией, никак не проверяя, является ли
// он вообще Goal. Комбинация с "wm_decay() никогда не вызывался" (см.
// фикс в subconscious.c) означала, что случайный узел (гипотеза,
// сущность, промежуточный результат синтеза), однажды активированный в
// ЛЮБОЙ предыдущей сессии на этом же долгоживущем core-процессе, мог
// НАВСЕГДА занимать "top goal" слот. CorePlanner для такого узла не
// находит HAS_ALGORITHM, тут же уходит в halt через branch_if_empty — и
// РЕАЛЬНАЯ цель (например InductiveSynthesisGoal) никогда даже не
// рассматривается, при этом MainLoop как ни в чём не бывало возвращает
// VM_OK. Именно так синтез правила "молча" не происходил.
//
// Проверяем ДВА независимых сигнала типизации:
//   1) сам atom с id==candidate имеет PROC_KIND_GOAL (путь
//      perception.c::perceive_hyper_json, "kind":"goal");
//   2) существует IS_A(candidate, "Goal") (путь activate_goal() /
//      cmd_execute.c::activate_goal_txn_fn — используется ВСЕМИ
//      существующими вызывающими: core/sdk.py, app/tools/interact.py).
// Поиск через idx_args[candidate] — ограничен связями самого кандидата
// (WM count <= 256), НЕ полный скан IS_A.
static bool wm_node_is_typed_goal(MDB_txn *txn, HyperMemory *hmem, node_id_t candidate) {
    if (!hmem || !txn) return true; // без HyperMemory не можем проверить — не блокируем планировщик

    MDB_val self_key = { sizeof(node_id_t), &candidate };
    MDB_val self_data;
    if (mdb_get(txn, hmem->dbi_atoms, &self_key, &self_data) == MDB_SUCCESS &&
        self_data.mv_size == sizeof(NeuroAtom)) {
        NeuroAtom self_atom;
        memcpy(&self_atom, self_data.mv_data, sizeof(NeuroAtom));
        if (proc_kind(self_atom.process_id) == PROC_KIND_GOAL)
            return true;
    }

    static node_id_t is_a_proc = 0;
    static node_id_t goal_concept = 0;
    if (is_a_proc == 0) {
        is_a_proc = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
        goal_concept = djb2_hash("Goal");
    }

    NeuroAtom *atoms = NULL;
    size_t count = 0;
    bool is_goal = false;

    if (hyper_find_by_participant(txn, hmem, candidate, 0, &atoms, &count) == 0) {
        for (size_t i = 0; i < count; i++) {
            if (atoms[i].process_id != is_a_proc) continue;
            if (HYPER_GET_TYPE(atoms[i].args[0].raw) != HYPER_TYPE_REF ||
                HYPER_GET_ID(atoms[i].args[0].raw) != candidate) continue;
            if (HYPER_GET_TYPE(atoms[i].args[1].raw) == HYPER_TYPE_REF &&
                HYPER_GET_ID(atoms[i].args[1].raw) == goal_concept) {
                is_goal = true;
                break;
            }
        }
        free(atoms);
    }
    return is_goal;
}

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

        // См. комментарий над wm_node_is_typed_goal(): не даём немаркированному
        // как Goal узлу перехватывать планировщик.
        if (ctx->hyper_mem && !wm_node_is_typed_goal(ctx->memory.txn, ctx->hyper_mem, candidate))
            continue;

        max_activation = act;
        top_node = candidate;
    }
    wm_unlock(ctx->memory.wm);

    if (top_node != 0) {
        ctx->reg[r_goal].type  = REG_NODE;
        ctx->reg[r_goal].node  = top_node;
        ctx->reg[r_found].type = REG_INT;
        ctx->reg[r_found].i    = 1;
        LOG_PLANNER("[CorePlanner] top goal selected: id=%lu activation=%.3f",
                    (unsigned long)top_node, max_activation);
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
        LOG_PLANNER("[CorePlanner] algorithm selected for goal=%lu: algo=%lu ucb=%.3f",
                    (unsigned long)goal_id, (unsigned long)best_algo, best_ucb);
    } else {
        ctx->reg[r_count].type = REG_INT;
        ctx->reg[r_count].i = 0;
        LOG_PLANNER("[CorePlanner] no HAS_ALGORITHM candidate for goal=%lu", (unsigned long)goal_id);
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

    LOG_PLANNER("[CorePlanner] dispatching goal=%lu via algo=%lu (async, cooldown=%ds)",
                (unsigned long)goal_id, (unsigned long)algo_id, GOAL_DISPATCH_COOLDOWN_SEC);

    vm_pool_submit(algo, goal_id, algo_id);
    return VM_OK;
}
