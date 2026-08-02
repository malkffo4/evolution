// core/src/reasoning/planner.c
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#include "types/id.h"
#include "memory/working.h"
#include "reasoning/analogy.h"
#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/string_pool/string_pool.h"
#include "storage/graph/graph.h"
#include "math/hash.h"
#include "planner.h"
#include "runtime/logging/logging.h"

// Добавить структуру кэша проваленных целей или писать cooldown прямо в свойство Node
#define GOAL_COOLDOWN_SEC 10

typedef struct {
    uint64_t goal_id;
    time_t ignore_until;
} GoalCooldown;

static GoalCooldown cooldown_list[128];

// Проверка перед попыткой построить план (теперь глобальная)
bool is_goal_on_cooldown(uint64_t goal_id) {
    time_t now = time(NULL);
    for (int i = 0; i < 128; i++) {
        if (cooldown_list[i].goal_id == goal_id && cooldown_list[i].ignore_until > now) {
            return true;
        }
    }
    return false;
}

void set_goal_cooldown(uint64_t goal_id) {
    time_t now = time(NULL);
    int empty_idx = -1;
    for (int i = 0; i < 128; i++) {
        if (cooldown_list[i].goal_id == goal_id) {
            cooldown_list[i].ignore_until = now + GOAL_COOLDOWN_SEC;
            return;
        }
        if (cooldown_list[i].goal_id == 0 && empty_idx == -1) empty_idx = i;
    }
    if (empty_idx != -1) {
        cooldown_list[empty_idx].goal_id = goal_id;
        cooldown_list[empty_idx].ignore_until = now + GOAL_COOLDOWN_SEC;
    }
}

void clear_goal_cooldown(uint64_t goal_id) {
    for (int i = 0; i < 128; i++) {
        if (cooldown_list[i].goal_id == goal_id) {
            cooldown_list[i].ignore_until = 0;
            return;
        }
    }
}

void planner_evaluate_goals(WorkingMemory *wm, void *txn) {
    if (!wm || !txn) return;

    for (uint32_t i = 0; i < wm->count; i++) {
        WorkingNode *node = &wm->nodes[i];

        if (is_goal_on_cooldown(node->node_id))
            continue;

        // 1. ИДЕНТИФИКАЦИЯ ЦЕЛИ
        // Если узел сильно активен и имеет высокую Полезность (Utility), он становится Целью.
        if (node->activation > 0.6f && node->state.usefulness > 0.7f) {
            // Чтобы не зацикливаться, проверяем, не фокусировались ли мы на нем только что
            if (node->focus_level > 5) continue;
            node->focus_level++;

            const char *goal_name = get_string_from_pool(txn, node->node_id);
            if (!goal_name) continue;

            LOG_PLANNER("[ПЛАНИРОВЩИК] Поставлена цель: '%s' (ID %lu). Строю план достижения...", goal_name, node->node_id);

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

                        // НЕ активируем подцель, если она на кулдауне
                        if (is_goal_on_cooldown(required_step_id)) {
                            const char *step_name = get_string_from_pool(txn, required_step_id);
                            LOG_PLANNER("[ПЛАНИРОВЩИК] Подцель '%s' на кулдауне, пропускаем.", step_name ? step_name : "UNKNOWN");
                            continue;
                        }

                        float step_weight = incoming_edges.items[j].confidence;
                        const char *step_name = get_string_from_pool(txn, required_step_id);
                        LOG_PLANNER("[ПОДЦЕЛЬ] Чтобы достичь '%s', нужно активировать '%s' (Уверенность: %.2f)",
                                    goal_name, step_name ? step_name : "UNKNOWN", step_weight);

                        wm_activate(wm, required_step_id, node->activation * 0.9f, node->state.usefulness * 0.9f);
                        action_found = 1;
                    }
                }
                free(incoming_edges.items);
            }

            // 4. РЕАКЦИЯ НА НЕХВАТКУ ЗНАНИЙ (Обучение / Изменение модели мира)
            if (!action_found) {
                node->state.novelty = 1.0f;
                LOG_PLANNER("[ТУПИК] У меня нет готового паттерна действий для '%s'. Ищу аналогии...", goal_name);

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
                        LOG_PLANNER("[ГИПОТЕЗА] %s (сходство %.2f)", hypothesis_label, candidates[k].score.total);
                    }
                    free(candidates);
                    set_goal_cooldown(node->node_id);
                    node->activation = 0.5f;
                }
            }
       }
    }
}
