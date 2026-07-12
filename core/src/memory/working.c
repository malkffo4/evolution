// memory/working.c
#include <stdlib.h>
#include <string.h>

#include "reasoning/engine.h"
#include "working.h"
#include "storage/db/db.h"
#include "math/hash.h"
#include "storage/node/node.h"
#include "storage/edge/edge.h"
#include "types/id.h"

// =========================================================================
// 1. ИНИЦИАЛИЗАЦИЯ И РАБОТА СО СЛОВАРЯМИ (Динамические свойства)
// Thinking Engine and Dynamic Properties
// =========================================================================

int wm_init(WorkingMemory *wm, uint32_t node_cap, uint32_t edge_cap) {
    wm->active_nodes.items = malloc(node_cap * sizeof(node_id_t));
    if (!wm->active_nodes.items) {
        // LOG_ERROR
        return 1;
    }
    wm->active_nodes.count = 0;
    wm->active_nodes.capacity = node_cap;
    wm->active_edges.items = malloc(edge_cap * sizeof(Edge));
    if (!wm->active_edges.items) {
        // LOG_ERROR
        free(wm->active_nodes.items);
        return 1;
    }
    wm->active_edges.count = 0;
    wm->active_edges.capacity = edge_cap;

    return 0;
}

void wm_clear(WorkingMemory *wm) {
    if (!wm)
        return;

    if (wm->active_nodes.items)
        free(wm->active_nodes.items);

    if (wm->active_edges.items)
        free(wm->active_edges.items);

    wm->active_nodes.count = 0;
    wm->active_edges.count = 0;
}
// =========================================================================
// 2. БАЗОВАЯ КОГНИЦИЯ (Загрузка, Активация, Забывание)
// =========================================================================
// Добавить узел в фокус внимания ИИ
void wm_activate(WorkingMemory *wm, uint64_t node_id, float activation, float base_emotion) {
    if (!wm) return;

    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].node_id == node_id) {
            wm->nodes[i].activation += activation;
            if (wm->nodes[i].activation > 1.0f) wm->nodes[i].activation = 1.0f;
            return;
        }
    }

    if (wm->count < 100) {
        wm->nodes[wm->count].node_id = node_id;
        wm->nodes[wm->count].activation = activation;
        wm->nodes[wm->count].focus_level = 0;
        // По умолчанию всё новое вызывает легкое любопытство
        wm->nodes[wm->count].state.novelty = base_emotion > 0 ? base_emotion : 0.5f;
        wm->nodes[wm->count].properties = NULL;
        wm->nodes[wm->count].attention_weight = base_emotion;
        wm->count++;
    }
}

// Затухание внимания (чтобы база не забивала проц старыми мыслями)
void wm_decay(WorkingMemory *wm) {
    if (!wm) return;
    for (uint32_t i = 0; i < wm->count; i++) {
        wm->nodes[i].activation *= 0.9f; // Угасание мысли на 10% каждый тик
        wm->nodes[i].state.urgency *= 0.8f;

        // Если узел совсем остыл - удаляем его свойства и вычищаем из памяти
        if (wm->nodes[i].activation < 0.05f) {
            DynamicProperty *curr = wm->nodes[i].properties;
            while (curr) {
                DynamicProperty *next = curr->next;
                free(curr);
                curr = next;
            }

            // Сдвигаем массив
            wm->nodes[i] = wm->nodes[wm->count - 1];
            wm->count--;
            i--; // Повторяем проверку для сдвинутого элемента
        }
    }
}

// =========================================================================
// 3. МАТРИЧНОЕ РАСПРОСТРАНЕНИЕ СМЫСЛОВ (L1 Cache Optimized)
// =========================================================================
void engine_spread_activation(WorkingMemory *wm, void *lmdb_txn) {
    (void)lmdb_txn; // В будущем здесь будет запрос к LMDB для подтягивания соседей
    if (!wm || wm->count < 2) return;

    // Сверхбыстрый цикл передачи эмоций между активными узлами (Закон Хебба)
    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].activation < 0.2f) continue;

        for (uint32_t j = i + 1; j < wm->count; j++) {
            if (wm->nodes[j].activation < 0.2f) continue;

            // Если два узла активны одновременно, они делятся когнитивным зарядом
            float transfer = wm->nodes[i].activation * wm->nodes[j].activation * 0.1f;

            // Обмен опасностью (если узел А опасен, узел Б заражается подозрением)
            if (wm->nodes[i].state.danger > 0.5f) {
                wm->nodes[j].state.danger += transfer;
            }
            // Обмен полезностью
            if (wm->nodes[j].state.usefulness > 0.5f) {
                wm->nodes[i].state.usefulness += transfer;
            }
        }
    }
}

// Установка динамического свойства (Аналог словаря в Python)
void wm_set_property(WorkingMemory *wm, uint64_t node_id, const char *key, const char *value) {
    if (!wm || !key || !value) return;

    uint64_t key_hash = djb2_hash(key);

    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].node_id == node_id) {
            // Ищем, нет ли уже такого ключа
            DynamicProperty *curr = wm->nodes[i].properties;
            while (curr) {
                if (curr->key_hash == key_hash) {
                    strncpy(curr->value, value, sizeof(curr->value) - 1);
                    return; // Обновили существующий
                }
                curr = curr->next;
            }

            // Если ключа нет, выделяем память под новое свойство
            DynamicProperty *new_prop = calloc(1, sizeof(DynamicProperty));
            if (!new_prop) return;
            new_prop->key_hash = key_hash;
            strncpy(new_prop->value, value, sizeof(new_prop->value) - 1);

            // Вставляем в начало списка
            new_prop->next = wm->nodes[i].properties;
            wm->nodes[i].properties = new_prop;
            return;
        }
    }
}

// Чтение свойства за O(N) по связному списку (где N - количество свойств одного узла, обычно < 10)
const char* wm_get_property(WorkingMemory *wm, uint64_t node_id, const char *key) {
    if (!wm || !key) return NULL;
    uint64_t key_hash = djb2_hash(key);

    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].node_id == node_id) {
            DynamicProperty *curr = wm->nodes[i].properties;
            while (curr) {
                if (curr->key_hash == key_hash) return curr->value;
                curr = curr->next;
            }
            break;
        }
    }
    return NULL;
}
