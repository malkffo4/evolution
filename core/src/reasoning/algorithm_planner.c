// reasoning/algorithm_planner.c
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "algorithm_planner.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "storage/vector_store/vector_store.h"
#include "runtime/vm/vm_context.h"
#include "runtime/logging/logging.h"
#include "runtime/ops/opcode.h"
#include "math/hash.h"
#include "memory/critic_state.h"
#include "knowledge/evaluation.h"

// Ищет все идентификаторы процессов, которые связывают Goal и Algorithm.
// Возвращает количество найденных отношений.
size_t find_goal_algorithm_relations(HyperMemory *hmem, node_id_t *rel_ids, size_t max_rels) {
    // ИСПРАВЛЕНИЕ: атомы из perceive_hyper_json() (Python "learn") всегда
    // проходят через proc_make(hash, kind) для собственного process_id.
    // IS_A-мета-триплет должен быть явно помечен "kind":"relation" на
    // стороне Python (см. патч bootstrap.py ниже) — здесь ищем именно
    // PROC_KIND_RELATION-версию хэша.
    node_id_t is_a = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
    node_id_t goal_algo_rel = djb2_hash("GoalAlgorithmRelation"); // значение-аргумент, не process_id — остаётся "сырым"

    NeuroAtom *isa_atoms = NULL;
    size_t isa_count = 0;
    if (hyper_find_by_process(hmem, is_a, 0, 0, &isa_atoms, &isa_count) != 0 || !isa_atoms)
        return 0;

    size_t found = 0;
    for (size_t i = 0; i < isa_count && found < max_rels; i++) {
        if (isa_atoms[i].args[1].raw != goal_algo_rel) continue;

        // args[0] хранит ИМЯ отношения как значение (просто хэш строки).
        // Сами атомы этого отношения (HAS_ALGORITHM(...)), если тоже
        // помечены "kind":"relation", имеют process_id = proc_make(...).
        node_id_t rel_name_hash = isa_atoms[i].args[0].raw;
        if (rel_name_hash != 0) {
            rel_ids[found++] = proc_make(rel_name_hash, PROC_KIND_RELATION);
        }
    }
    free(isa_atoms);
    return found;
}

/* ---------------------------------------------------------------- */
/* Вспомогательная функция: поиск всех алгоритмов, связанных с целью */
/* ---------------------------------------------------------------- */
static int find_algorithms_for_goal(HyperMemory *hmem, node_id_t goal_id, node_id_t *out, int max_out) {
    if (!hmem || !out) return 0;

    node_id_t rel_has_algo = proc_make(djb2_hash("HAS_ALGORITHM"), PROC_KIND_RELATION);
    NeuroAtom *atoms = NULL;
    size_t count = 0;

    // Ищем все атомы, где process_id == HAS_ALGORITHM и один из аргументов == goal_id
    int rc = hyper_find_by_process(hmem, rel_has_algo, 0, 0, &atoms, &count);
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
/* Выбор лучшего алгоритма по статистике               */
/* ---------------------------------------------------------------- */
static node_id_t pick_best(VMContext *ctx, node_id_t *candidates, int count) {
    if (count == 0) return 0;
    if (count == 1) return candidates[0];

    node_id_t best_algo = candidates[0];
    float best_ucb = -1.0f;
    float exploration_param = 0.5f; // C-коэффициент жажды знаний

    for (int i = 0; i < count; i++) {
        // Извлекаем оценку алгоритма из HyperMemory.
        // Пока мы не внедрили полноценный механизм Score/Trust,
        // имитируем логику: ищем атом самого алгоритма и смотрим его уверенность.
        NeuroAtom algo_atom;
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

/* ---------------------------------------------------------------- */
/* Главная функция планировщика                                      */
/* ---------------------------------------------------------------- */
int planner_select_algorithm(HyperMemory *hmem, node_id_t goal_id, VMContext *ctx, node_id_t *out_algo_id) {
    if (!hmem || !out_algo_id) return -1;

    node_id_t candidates[MAX_CANDIDATES_ALGO];
    int cand_count = 0;

    // 1. Прямой поиск в гипер-атомах
    cand_count = find_algorithms_for_goal(hmem, goal_id, candidates, MAX_CANDIDATES_ALGO);

    // 2. Если не нашли — ищем похожие цели (только если есть эмбеддинг)
    if (cand_count == 0) {
        float query_emb[EMBEDDING_DIM];
        if (load_embedding(ctx->memory.txn, goal_id, query_emb) == 0) { // транзакция не нужна, берём из кеша или глобальной БД
            uint64_t similar_goals[8];
            int sim_count = find_similar_nodes(ctx->memory.txn, query_emb, 8, similar_goals); // здесь txn=NULL, если load_embedding работает без txn
            if (sim_count > 0) {
                for (int i = 0; i < sim_count && cand_count < MAX_CANDIDATES_ALGO; i++) {
                    cand_count += find_algorithms_for_goal(hmem, similar_goals[i],
                                                          &candidates[cand_count],
                                                          MAX_CANDIDATES_ALGO - cand_count);
                }
            }
        }
    }

    if (cand_count == 0) return -2; // алгоритм не найден

    *out_algo_id = pick_best(ctx, candidates, cand_count);
    return 0;
}

int planner_select_all_algorithms(HyperMemory *hmem, node_id_t goal_id, VMContext *ctx,
                                  node_id_t *candidates, int *cand_count) {
    (void)ctx;
    *cand_count = find_algorithms_for_goal(hmem, goal_id, candidates, MAX_CANDIDATES_ALGO);
    return (*cand_count > 0) ? 0 : -2;
}
