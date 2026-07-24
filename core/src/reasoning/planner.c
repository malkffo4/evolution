// reasoning/planner.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "types/id.h"
#include "memory/working.h"
#include "reasoning/analogy.h"
#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/string_pool/string_pool.h"
#include "storage/graph/graph.h"
#include "math/hash.h"
#include "planner.h"

// Добавить структуру кэша проваленных целей или писать cooldown прямо в свойство Node
#define GOAL_COOLDOWN_SEC 10

typedef struct {
    uint64_t goal_id;
    time_t ignore_until;
} GoalCooldown;

static GoalCooldown cooldown_list[128];

// Проверка перед попыткой построить план
static bool is_goal_on_cooldown(uint64_t goal_id) {
    time_t now = time(NULL);
    for (int i = 0; i < 128; i++) {
        if (cooldown_list[i].goal_id == goal_id && cooldown_list[i].ignore_until > now) {
            return true;
        }
    }
    return false;
}

static void set_goal_cooldown(uint64_t goal_id) {
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
// TODO
// Псевдокод интеграции в ваш цикл планировщика:
// void planner_evaluate_goals() {
//     // Получить активные цели...
//     for (int i = 0; i < num_goals; i++) {
//         uint64_t current_goal = goals[i].id;

//         if (is_goal_on_cooldown(current_goal)) {
//             continue; // Пропускаем, чтобы не спамить и сберечь CPU
//         }

//         LOG_INFO("[ПЛАНИРОВЩИК] Поставлена цель: ID %llu. Строю план...", current_goal);

//         int rc = build_plan_for_goal(current_goal);

//         if (rc == PLAN_DEAD_END) {
//             LOG_WARN("  -> [ТУПИК] У меня нет готового паттерна действий. Ищу аналогии...");

//             // Запускаем поиск аналогий (Hypothesis By Analogy)
//             rc = try_hypothesis_by_analogy(current_goal);

//             if (rc == HYPOTHESIS_FAILED) {
//                 // Если даже аналогии не помогли - замораживаем цель на время
//                 set_goal_cooldown(current_goal);
//             }
//         }
//     }
// }

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
                        float step_weight = incoming_edges.items[j].confidence;

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

// Функция извлечения алгоритма мышления из LMDB и передачи в VM
// int evaluate_behavioral_pattern(VMContext* ctx, node_id_t behavior_id) {
//     // 1. Вытаскиваем граф логики из базы данных (LMDB)
//     graph_path_t* path = graph_get_execution_path(behavior_id);
//     if (!path) {
//         // Логика не найдена, прерываем выполнение
//         return VM_STATUS_ERROR_NOT_FOUND;
//     }

//     // 2. Компилятор превращает путь графа в примитивные опкоды VM
//     // Например: узлы графа превращаются в опкоды LOAD, MATCH, BRANCH
//     cognitive_chain_t chain;
//     int compile_status = compiler_build_chain(path, &chain.bytecode, &chain.instruction_count);

//     if (compile_status != SUCCESS) {
//         return VM_STATUS_ERROR_COMPILE;
//     }

//     // 3. Загружаем скомпилированную цепочку в рабочую память VM
//     vm_load_program(ctx, chain.bytecode, chain.instruction_count);

//     // 4. Запускаем выполнение гипотезы/цепочки
//     int exec_status = vm_execute(ctx);

//     // 5. Оценка результата (например, найдена ли уязвимость в симуляции)
//     if (exec_status == VM_STATUS_VULNERABILITY_FOUND) {
//         // Запись нового обнаруженного паттерна обратно в базу (самообучение)
//         knowledge_record_hypothesis(ctx->memory, behavior_id, "VULN_CONFIRMED");
//     }

//     // Очистка памяти арены
//     arena_free(chain.bytecode);

//     return exec_status;
// }
