// planner — решает, каких знаний не хватает, и ставит задачи на их получение.
// reasoning/learning_planner.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types/id.h"
#include "memory/working.h"
#include "reasoning/analogy.h"
#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/string_pool/string_pool.h"
#include "storage/graph/graph.h"
#include "math/hash.h"
// =========================================================================
// ПЛАНИРОВЩИК (Целеполагание и разбиение задач)
// =========================================================================
// Этот модуль реализует слой: Планирование -> Действие
// Он ищет в Рабочей памяти узлы, которые система "хочет" достичь (высокий usefulness),
// и разворачивает их в цепочку необходимых действий на основе графа LMDB.

void planner_evaluate_goals(WorkingMemory *wm, void *txn) {
    if (!wm || !txn) return;

    for (uint32_t i = 0; i < wm->count; i++) {
        WorkingNode *node = &wm->nodes[i];

        // 1. ИДЕНТИФИКАЦИЯ ЦЕЛИ
        // Если узел сильно активен и имеет высокую Полезность (Utility), он становится Целью.
        if (node->activation > 0.6f && node->state.usefulness > 0.7f) {

            // Чтобы не зацикливаться, проверяем, не фокусировались ли мы на нем только что
            if (node->focus_level > 5) continue;
            node->focus_level++;

            const char *goal_name = get_string_from_pool(txn, node->node_id);
            if (!goal_name) continue;

            printf("\n\033[36m[ПЛАНИРОВЩИК] Поставлена цель: '%s' (ID %lu). Строю план достижения...\033[0m\n", goal_name, node->node_id);

            EdgeList incoming_edges = {0};
            int action_found = 0;

            // 2. ОБРАТНЫЙ ВЫВОД (Backward Chaining)
            // Ищем в памяти, ЧТО приводит к этой цели. Мы ищем ребра ВХОДЯЩИЕ в цель.
            if (get_edges_to_node(txn, node->node_id, &incoming_edges) == MDB_SUCCESS && incoming_edges.items) {

                for (uint32_t j = 0; j < incoming_edges.count; j++) {
                    const char *relation_name = get_string_from_pool(txn, incoming_edges.items[j].key.relation);

                    // Эвристика: ищем причинно-следственные связи (CAUSES, LEADS_TO, ACHIVES)
                    // В будущем семантический процессор будет сам определять смысл связи
                    if (relation_name && (strcasestr(relation_name, "cause") || strcasestr(relation_name, "achieve") || strcasestr(relation_name, "приводит"))) {

                        uint64_t required_step_id = incoming_edges.items[j].key.source;
                        uint32_t step_weight = incoming_edges.items[j].evidence_count;

                        const char *step_name = get_string_from_pool(txn, required_step_id);

                        printf("  -> [ПОДЦЕЛЬ] Чтобы достичь '%s', нужно активировать '%s' (Уверенность: %.2f)\n",
                               goal_name, step_name ? step_name : "UNKNOWN", step_weight);

                        // 3. ФОРМИРОВАНИЕ ПЛАНА (Дерево подцелей)
                        // Заряжаем необходимое действие/подцель в Рабочую Память.
                        // Если это действие, на следующем тике Движок Мышления запустит его.
                        // Если это абстракция, Планировщик разобьет её еще глубже.
                        wm_activate(wm, required_step_id, node->activation * 0.9f, node->state.usefulness * 0.9f);
                        action_found = 1;
                    }
                }
                free(incoming_edges.items);
            }

            // 4. РЕАКЦИЯ НА НЕХВАТКУ ЗНАНИЙ (Обучение / Изменение модели мира)
            if (!action_found) {
                node->state.novelty = 1.0f;
                printf("  -> [ТУПИК] У меня нет готового паттерна действий для '%s'. Ищу аналогии...\n", goal_name);

                // Вызов поиска аналогий
                AnalogyCandidate *candidates = NULL;
                int cand_count = 0;
                if (find_analogous_patterns(txn, node->node_id, &candidates, &cand_count) == 0) {
                    for (int k = 0; k < cand_count; k++) {
                        // Создаём узел-гипотезу в графе
                        char hypothesis_label[256];
                        snprintf(hypothesis_label, sizeof(hypothesis_label),
                                "hypothesis_%lu_%lu", node->node_id, candidates[k].analogous_node);
                        uint64_t hyp_id = djb2_hash(hypothesis_label);
                        Node hyp_node = {
                            .id = hyp_id,
                            .name_hash = add_string_to_pool(txn, hypothesis_label),
                            .simhash = 0
                        };
                        create_node(txn, &hyp_node);

                        // Связываем гипотезу с исходным узлом и аналогом
                        //create_flexible_edge(txn, node->node_id, hyp_id, "HAS_HYPOTHESIS", 0);
                        //create_flexible_edge(txn, hyp_id, candidates[k].analogous_node_id, "BASED_ON", 0);

                        // Добавляем задачу на проверку в очередь (можно через файл или IPC)
                        // Пока просто выводим
                        printf("  -> [ГИПОТЕЗА] %s (сходство %.2f)\n", hypothesis_label, candidates[k].score.total);
                    }
                    free(candidates);
                }
            }
       }
    }
}
