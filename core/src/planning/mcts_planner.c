#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "reasoning/engine.h"
#include "storage/db.h"

#define EXPLORATION_CONSTANT 1.4142f

typedef struct AttackNode {
    uint64_t state_hash;      // Хэш состояния системы (например, "папка существует")
    uint64_t action_id;       // ID действия (например, djb2 от "mkdir target")
    int visits;               // Число проходов через этот узел
    float total_reward;       // Накопленная награда
    struct AttackNode *parent;
    struct AttackNode *children[32];
    int child_count;
} AttackNode;

// Очистка оперативной памяти от остывших узлов
void wm_cleanup(WorkingMemory *wm, float threshold) {
    uint32_t write_idx = 0;
    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].activation > threshold) {
            wm->nodes[write_idx++] = wm->nodes[i];
        } else {
            // Освобождаем динамические свойства удаляемого узла
            DynamicProperty *curr = wm->nodes[i].properties;
            while (curr) {
                DynamicProperty *next = curr->next;
                free(curr);
                curr = next;
            }
        }
    }
    if (wm->count != write_idx) {
        printf("[WM CLEANUP] Удалено %d неактуальных узлов из оперативной памяти.\n", wm->count - write_idx);
        wm->count = write_idx;
    }
}

// Расчет критерия UCB1 (Upper Confidence Bound) для баланса исследования и эксплуатации
float calculate_ucb(AttackNode *node) {
    if (node->visits == 0) return 99999.0f; // Всегда приоритетно исследуем новые ветки

    float exploitation = node->total_reward / (float)node->visits;
    float exploration = EXPLORATION_CONSTANT * sqrtf(logf((float)node->parent->visits) / (float)node->visits);

    return exploitation + exploration;
}

// Выбор наиболее перспективной ветки в дереве симуляции
AttackNode* select_best_attack_path(AttackNode *root) {
    AttackNode *current = root;
    while (current->child_count > 0) {
        float best_score = -1.0f;
        AttackNode *best_child = NULL;
        for (int i = 0; i < current->child_count; i++) {
            float score = calculate_ucb(current->children[i]);
            if (score > best_score) {
                best_score = score;
                best_child = current->children[i];
            }
        }
        if (!best_child) break;
        current = best_child;
    }
    return current;
}

// Симуляция успешности выполнения команды.
// Проверяет реальное состояние ОС или обращается к накопленной базе опыта LMDB.
float simulate_attack_success(AttackNode *node, WorkingMemory *wm, MDB_txn *txn) {
    (void)wm;

    // Пытаемся получить строковое имя действия
    const char *action_cmd = get_string_from_pool(txn, node->action_id);
    if (!action_cmd) {
        return 0.1f; // Действие неизвестно
    }

    float score = 0.5f;

    // Реалистичная оценка: если действие - это "mkdir test_dir"
    if (strstr(action_cmd, "mkdir")) {
        // Проверяем, существует ли уже такая папка. Если да, повторный mkdir выдаст ошибку
        char *dir_name = strchr(action_cmd, ' ');
        if (dir_name) {
            dir_name++; // Пропускаем пробел
            struct stat st = {0};
            if (stat(dir_name, &st) == 0) {
                // Папка уже существует, награда за дублирование mkdir минимальна
                score = 0.1f;
            } else {
                // Всё чисто, высокая вероятность успеха
                score = 0.9f;
            }
        }
    }

    // Проверяем наличие ребра "LEADS_TO_ERROR" в нашей базе знаний
    Edge penalty_edge;
    uint64_t fail_state_id = djb2_hash("FAILURE_STATE");
    if (get_edge(txn, node->action_id, djb2_hash("LEADS_TO_ERROR"), fail_state_id, &penalty_edge) == MDB_SUCCESS) {
        // Корректируем вероятность успеха на основе негативного опыта
        score -= penalty_edge.confidence * 0.8f;
        if (score < 0.0f) score = 0.0f;
    }

    free(action_cmd);
    return score;
}

// Обратное распространение награды по дереву решений
void backpropagate_reward(AttackNode *node, float reward) {
    while (node != NULL) {
        node->visits += 1;
        node->total_reward += reward;
        node = node->parent;
    }
}

// Главный цикл выбора действия для достижения терминального состояния цели
uint64_t mcts_think_next_move(uint64_t target_state_id, WorkingMemory *wm, MDB_txn *txn) {
    // Чистим мусор в оперативной памяти перед расчетами
    wm_cleanup(wm, 0.1f);

    printf("\n\033[33m[MCTS ПЛАНИРОВЩИК] Анализ путей решения задачи для цели ID: %lu...\033[0m\n", target_state_id);

    AttackNode root = { .state_hash = target_state_id, .visits = 1, .total_reward = 0, .child_count = 0 };

    // Эмуляция веток возможных действий
    // В полной версии эти ветки динамически расширяются на основе связей из LMDB
    AttackNode step_mkdir = {
        .state_hash = target_state_id,
        .action_id = djb2_hash("mkdir test_dir"),
        .visits = 0,
        .total_reward = 0.0f,
        .parent = &root,
        .child_count = 0
    };

    root.children[0] = &step_mkdir;
    root.child_count = 1;

    // 100 циклов симуляции в "уме"
    for (int i = 0; i < 100; i++) {
        AttackNode *leaf = select_best_attack_path(&root);
        float reward = simulate_attack_success(leaf, wm, txn);
        backpropagate_reward(leaf, reward);
    }

    float max_reward = -1.0f;
    uint64_t best_action = 0;

    for (int i = 0; i < root.child_count; i++) {
        float avg_reward = root.children[i]->total_reward / (float)root.children[i]->visits;
        if (avg_reward > max_reward) {
            max_reward = avg_reward;
            best_action = root.children[i]->action_id;
        }
    }

    printf("\033[32m[MCTS ПЛАНИРОВЩИК] Оптимальное системное действие выбрано. Надежность стратегии: %.2f%%\033[0m\n", max_reward * 100.0f);
    return best_action;
}
