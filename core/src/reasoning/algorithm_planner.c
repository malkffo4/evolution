// reasoning/algorithm_planner.c
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>

#include "algorithm_planner.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "storage/vector_store/vector_store.h"
#include "storage/db/db.h"
#include "runtime/vm/vm_context.h"
#include "runtime/logging/logging.h"
#include "runtime/ops/opcode.h"
#include "math/hash.h"
#include "memory/critic_state.h"
#include "knowledge/evaluation.h"

#define GOAL_ALGO_REL_CACHE_MAX 16
static node_id_t g_goal_algo_rel_cache[GOAL_ALGO_REL_CACHE_MAX];
static size_t    g_goal_algo_rel_cache_count = 0;
static bool       g_goal_algo_rel_cache_valid = false;

void invalidate_goal_algorithm_relation_cache(void) {
    g_goal_algo_rel_cache_valid = false;
}

// Ищет все идентификаторы процессов, которые связывают Goal и Algorithm.
// Возвращает количество найденных отношений.
size_t find_goal_algorithm_relations(MDB_txn *txn, HyperMemory *hmem, node_id_t *rel_ids, size_t max_rels) {
    if (g_goal_algo_rel_cache_valid) {
        size_t n = g_goal_algo_rel_cache_count < max_rels ? g_goal_algo_rel_cache_count : max_rels;
        memcpy(rel_ids, g_goal_algo_rel_cache, n * sizeof(node_id_t));
        return n;
    }

    node_id_t is_a = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
    node_id_t goal_algo_rel = djb2_hash("GoalAlgorithmRelation");

    NeuroAtom *isa_atoms = NULL;
    size_t isa_count = 0;
    // Единственный полный скан idx_process("IS_A") за весь процесс жизни
    // (до первого успеха). Раньше это происходило на КАЖДОМ тике MainLoop
    // × КАЖДОМ кандидате WM — O(1) амортизированно после первого вызова.
    if (hyper_find_by_process(txn, hmem, is_a, 0, 0, &isa_atoms, &isa_count) != 0 || !isa_atoms)
        return 0;

    size_t found = 0;
    for (size_t i = 0; i < isa_count && found < max_rels; i++) {
        if (isa_atoms[i].args[1].raw != goal_algo_rel) continue;
        node_id_t rel_name_hash = isa_atoms[i].args[0].raw;
        if (rel_name_hash != 0)
            rel_ids[found++] = proc_make(rel_name_hash, PROC_KIND_RELATION);
    }
    free(isa_atoms);

    if (found > 0) {
        size_t n = found < GOAL_ALGO_REL_CACHE_MAX ? found : GOAL_ALGO_REL_CACHE_MAX;
        memcpy(g_goal_algo_rel_cache, rel_ids, n * sizeof(node_id_t));
        g_goal_algo_rel_cache_count = n;
        g_goal_algo_rel_cache_valid = true;
    }
    return found;
}

/* ---------------------------------------------------------------- */
/* Вспомогательная функция: поиск всех алгоритмов, связанных с целью */
/* ---------------------------------------------------------------- */
static int find_algorithms_for_goal(MDB_txn *txn, HyperMemory *hmem, node_id_t goal_id, node_id_t *out, int max_out) {
    if (!hmem || !out) return 0;

    node_id_t rel_has_algo = proc_make(djb2_hash("HAS_ALGORITHM"), PROC_KIND_RELATION);
    NeuroAtom *atoms = NULL;
    size_t count = 0;

    // Ищем все атомы, где process_id == HAS_ALGORITHM и один из аргументов == goal_id
    int rc = hyper_find_by_process(txn, hmem, rel_has_algo, 0, 0, &atoms, &count);
    if (rc != 0 || !atoms || count == 0) return 0;

    int found = 0;
    for (size_t i = 0; i < count && found < max_out; i++) {
        NeuroAtom *a = &atoms[i];
        // Проверяем, есть ли goal_id среди аргументов (обычно первый аргумент – цель)
        for (int arg_idx = 0; arg_idx < HYPER_VAL_SLOTS; arg_idx++) {
            if (HYPER_GET_TYPE(a->args[arg_idx].raw) == HYPER_TYPE_REF && HYPER_GET_ID(a->args[arg_idx].raw) == goal_id) {
                // Второй аргумент (или другой, в зависимости от модели) — алгоритм
                for (int algo_idx = 0; algo_idx < HYPER_VAL_SLOTS; algo_idx++) {
                    if (algo_idx == arg_idx) continue;
                    if (HYPER_GET_TYPE(a->args[algo_idx].raw) == HYPER_TYPE_REF) {
                        node_id_t algo_id = HYPER_GET_ID(a->args[algo_idx].raw);
                        if (!is_quarantined(algo_id)) {
                            out[found++] = algo_id;
                        }
                        break;  // берём только один алгоритм на атом
                    }
                }
            }
        }
    }
    free(atoms);
    return found;
}

/* ---------------------------------------------------------------- */
/* Гомеостаз exploration_param: db.graph.properties, тот же паттерн, что
 * reasoning/strategy_store.c::prop_get_float() использует для весов
 * аналогии. Собственный "узел-контейнер" PLANNER_NODE_ID — не делит
 * ключи со STRAT_NODE_ID (тот статичен и приватен для strategy_store.c).
 * Пока ничего не ЗАПИСЫВАЕТ значение — только читает с фолбэком: до тех
 * пор, пока какой-нибудь тюнер (по образцу reasoning_weights_sgd_update)
 * не начнёт писать сюда, поведение планировщика идентично прежнему
 * захардкоженному 0.5f.
 */
/* ---------------------------------------------------------------- */
#define PLANNER_NODE_ID djb2_hash("Planner:AlgorithmSelection")

typedef struct { node_id_t nid; uint64_t hash; } PlannerPropKey;

static float planner_prop_get_float(MDB_txn *txn, uint64_t key_hash, float def) {
    if (!txn) return def;
    PlannerPropKey pk = { PLANNER_NODE_ID, key_hash };
    MDB_val key = { sizeof(pk), &pk };
    MDB_val data;
    if (mdb_get(txn, db.graph.properties, &key, &data) != MDB_SUCCESS) return def;
    if (data.mv_size < sizeof(NodeProperty)) return def;
    NodeProperty hdr;
    memcpy(&hdr, data.mv_data, sizeof(hdr));
    if (hdr.type != PROP_FLOAT || data.mv_size < sizeof(hdr) + sizeof(float)) return def;
    float v;
    memcpy(&v, (const char *)data.mv_data + sizeof(hdr), sizeof(float));
    return v;
}

/* ---------------------------------------------------------------- */
/* Выбор лучшего алгоритма по статистике               */
/* ---------------------------------------------------------------- */
static node_id_t pick_best(VMContext *ctx, node_id_t *candidates, int count) {
    if (count == 0) return 0;
    if (count == 1) return candidates[0];

    node_id_t best_algo = candidates[0];
    float best_ucb = -1.0f;
    // Было захардкожено: float exploration_param = 0.5f;
    // Теперь дрейфующий параметр гомеостаза (пока read-only с фолбэком —
    // см. комментарий у planner_prop_get_float выше).
    float exploration_param = planner_prop_get_float(ctx->memory.txn,
                                                       djb2_hash("exploration_param"),
                                                       0.5f);

    for (int i = 0; i < count; i++) {
        // Извлекаем оценку алгоритма из HyperMemory.
        // Пока мы не внедрили полноценный механизм Score/Trust,
        // имитируем логику: ищем атом самого алгоритма и смотрим его уверенность.
        float mean = 0.5f;       // Приор: 50% успешности
        float confidence = 0.01f; // Приор: данных почти нет

        MDB_val key = { sizeof(node_id_t), &candidates[i] };
        MDB_val data;
        if (mdb_get(ctx->memory.txn, ctx->hyper_mem->dbi_atoms, &key, &data) == MDB_SUCCESS) {
            NeuroAtom *a = (NeuroAtom*)data.mv_data;
            mean = a->truth_mean;
            confidence = a->truth_confidence;
        }

        // Фикс C3: Формула UCB = ожидаемая_награда + бонус_за_неопределенность
        // Чем меньше confidence, тем больше бонус -> ИИ попробует этот алгоритм
        float ucb = mean + exploration_param * sqrtf(1.0f / (confidence + 0.001f));

        LOG_DEBUG("[PLANNER] Algo %lu UCB=%.3f (mean=%.2f, conf=%.2f)",
                  (unsigned long)candidates[i], ucb, mean, confidence);

        if (ucb > best_ucb) {
            best_ucb = ucb;
            best_algo = candidates[i];
        }
    }

    return best_algo;
}

int planner_select_all_algorithms(HyperMemory *hmem, node_id_t goal_id, VMContext *ctx,
                                  node_id_t *candidates, int *cand_count) {
    *cand_count = find_algorithms_for_goal(ctx->memory.txn, hmem, goal_id, candidates, MAX_CANDIDATES_ALGO);
    return (*cand_count > 0) ? 0 : -2;
}
