// reasoning/algorithm_planner.c
#include <stdlib.h>
#include <string.h>

#include "algorithm_planner.h"
#include "storage/graph/graph.h"        // get_edges_from_node, EdgeList
#include "storage/vector_store/vector_store.h" // find_similar_nodes
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_types.h"        // VMProfile
#include "runtime/ops/opcode.h"
#include "math/hash.h"                           // djb2_hash

/**
 * Выбирает лучший алгоритм из кандидатов.
 *
 * На данном этапе использует агрегированную статистику профиля VM по оператору
 * OP_EXEC_ALGORITHM (количество ошибок). В будущем будет читать метрики
 * непосредственно из свойств узла алгоритма (confidence, success_count,
 * average_time и т.д.), накопленные Learning.
 */
static node_id_t pick_best(VMContext *ctx, node_id_t *candidates, int count) {
    node_id_t best = candidates[0];
    uint64_t best_failures = UINT64_MAX;

    for (int i = 0; i < count; i++) {
        // Пока все алгоритмы используют один и тот же оператор OP_EXEC_ALGORITHM,
        // но в будущем у каждого алгоритма будет собственная статистика.
        uint64_t failures = ctx->profile[OP_EXEC_ALGORITHM].failures;

        // Дополнительно можно учесть количество вызовов: чем больше вызовов при
        // малом числе ошибок, тем надёжнее алгоритм.
        uint64_t calls = ctx->profile[OP_EXEC_ALGORITHM].calls;
        if (calls > 0) {
            // Предпочтение отдаём алгоритмам с меньшей долей ошибок
            uint64_t score = (failures * 100) / calls;  // процент ошибок
            if (score < best_failures) {
                best_failures = score;
                best = candidates[i];
            }
        } else {
            // Нет статистики — берём первый попавшийся
            if (best_failures == UINT64_MAX) {
                best = candidates[i];
                best_failures = 0;
            }
        }
    }
    return best;
}

int planner_select_algorithm(MDB_txn *txn, node_id_t goal_id, VMContext *ctx, node_id_t *out_algo_id) {
    if (!txn || !out_algo_id) return -1;

    // Отношение "HAS_ALGORITHM" — захардкодим строку для простоты
    node_id_t rel_has_algo = djb2_hash("HAS_ALGORITHM");

    // 1. Прямой поиск алгоритмов для цели
    EdgeList edges = {0};
    int rc = get_edges_from_node(txn, goal_id, &edges);
    if (rc != MDB_SUCCESS) return rc;

    node_id_t candidates[MAX_CANDIDATES_ALGO];
    int cand_count = 0;

    for (uint32_t i = 0; i < edges.count && cand_count < MAX_CANDIDATES_ALGO; i++) {
        if (edges.items[i].key.relation == rel_has_algo) {
            candidates[cand_count++] = edges.items[i].key.target;
        }
    }
    free(edges.items);

    // 2. Если нет прямых — ищем похожие цели и заимствуем их алгоритмы
    if (cand_count == 0) {
        uint64_t similar_goals[8];
        int sim_count = find_similar_nodes(txn, NULL, 8, similar_goals); // передадим NULL как query_emb, пока заглушка
        // find_similar_nodes требует query_emb, поэтому временно используем прямой вызов с пустым вектором
        // В будущем заменим на search_by_node_id
        if (sim_count > 0) {
            for (int i = 0; i < sim_count && cand_count < MAX_CANDIDATES_ALGO; i++) {
                EdgeList sim_edges = {0};
                if (get_edges_from_node(txn, similar_goals[i], &sim_edges) == MDB_SUCCESS) {
                    for (uint32_t j = 0; j < sim_edges.count && cand_count < MAX_CANDIDATES_ALGO; j++) {
                        if (sim_edges.items[j].key.relation == rel_has_algo) {
                            candidates[cand_count++] = sim_edges.items[j].key.target;
                        }
                    }
                    free(sim_edges.items);
                }
            }
        }
    }

    if (cand_count == 0) return -2; // алгоритм не найден

    *out_algo_id = pick_best(ctx, candidates, cand_count);
    return 0;
}
