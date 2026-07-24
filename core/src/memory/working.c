// memory/working.c
#include <stdlib.h>
#include <string.h>

#include "working.h"
#include "storage/db/db.h"
#include "math/hash.h"
#include "storage/node/node.h"
#include "storage/edge/edge.h"
#include "types/id.h"

int wm_init(WorkingMemory *wm, uint32_t node_cap, uint32_t edge_cap) {
    wm->active_nodes.items = malloc(node_cap * sizeof(node_id_t));
    if (!wm->active_nodes.items) {
        return 1;
    }
    wm->active_nodes.count = 0;
    wm->active_nodes.capacity = node_cap;

    wm->active_edges.items = malloc(edge_cap * sizeof(Edge));
    if (!wm->active_edges.items) {
        free(wm->active_nodes.items);
        return 1;
    }
    wm->active_edges.count = 0;
    wm->active_edges.capacity = edge_cap;

    // ИСПРАВЛЕНИЕ: Выделяем память под WorkingNode и инициализируем емкость
    wm->capacity = 100; // Лимит, используемый в wm_activate
    wm->nodes = calloc(wm->capacity, sizeof(WorkingNode));
    if (!wm->nodes) {
        free(wm->active_edges.items);
        free(wm->active_nodes.items);
        return 1;
    }
    wm->count = 0;
    wm->tick = 0;

    return 0;
}

void wm_clear(WorkingMemory *wm) {
    if (!wm)
        return;
    if (wm->active_nodes.items)
        free(wm->active_nodes.items);
    if (wm->active_edges.items)
        free(wm->active_edges.items);
    if (wm->nodes) {
        // Очищаем динамические свойства каждого узла
        for (uint32_t i = 0; i < wm->count; i++) {
            DynamicProperty *curr = wm->nodes[i].properties;
            while (curr) {
                DynamicProperty *next = curr->next;
                free(curr);
                curr = next;
            }
        }
        free(wm->nodes);
    }
    wm->active_nodes.count = 0;
    wm->active_edges.count = 0;
    wm->count = 0;
}

void wm_activate(WorkingMemory *wm, uint64_t node_id, float activation, float base_emotion) {
    if (!wm || !wm->nodes) return;
    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].node_id == node_id) {
            wm->nodes[i].activation += activation;
            if (wm->nodes[i].activation > 1.0f) wm->nodes[i].activation = 1.0f;
            return;
        }
    }
    if (wm->count < wm->capacity) {
        wm->nodes[wm->count].node_id = node_id;
        wm->nodes[wm->count].activation = activation;
        wm->nodes[wm->count].focus_level = 0;
        wm->nodes[wm->count].state.novelty = base_emotion > 0 ? base_emotion : 0.5f;
        wm->nodes[wm->count].properties = NULL;
        wm->nodes[wm->count].attention_weight = base_emotion;
        wm->count++;
    }
}

void wm_decay(WorkingMemory *wm) {
    if (!wm || !wm->nodes) return;
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

void engine_spread_activation(WorkingMemory *wm, void *lmdb_txn) {
    (void)lmdb_txn;
    if (!wm || !wm->nodes || wm->count < 2) return;
    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].activation < 0.2f) continue;
        for (uint32_t j = i + 1; j < wm->count; j++) {
            if (wm->nodes[j].activation < 0.2f) continue;
            float transfer = wm->nodes[i].activation * wm->nodes[j].activation * 0.1f;
            if (wm->nodes[i].state.danger > 0.5f) {
                wm->nodes[j].state.danger += transfer;
            }
            if (wm->nodes[j].state.usefulness > 0.5f) {
                wm->nodes[i].state.usefulness += transfer;
            }
        }
    }
}

void wm_set_property(WorkingMemory *wm, uint64_t node_id, const char *key, const char *value) {
    if (!wm || !wm->nodes || !key || !value) return;
    uint64_t key_hash = djb2_hash(key);
    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].node_id == node_id) {
            DynamicProperty *curr = wm->nodes[i].properties;
            while (curr) {
                if (curr->key_hash == key_hash) {
                    strncpy(curr->value, value, sizeof(curr->value) - 1);
                    return;
                }
                curr = curr->next;
            }
            DynamicProperty *new_prop = calloc(1, sizeof(DynamicProperty));
            if (!new_prop) return;
            new_prop->key_hash = key_hash;
            strncpy(new_prop->value, value, sizeof(new_prop->value) - 1);
            new_prop->next = wm->nodes[i].properties;
            wm->nodes[i].properties = new_prop;
            return;
        }
    }
}

const char* wm_get_property(WorkingMemory *wm, uint64_t node_id, const char *key) {
    if (!wm || !wm->nodes || !key) return NULL;
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

node_id_t wm_get_highest_goal(WorkingMemory *wm) {
    if (!wm) return 0;
    node_id_t best_id = 0;
    float best_score = -1.0f;
    for (uint32_t i = 0; i < wm->count; i++) {
        WorkingNode *n = &wm->nodes[i];
        // Простая эвристика цели: высокая активация и полезность
        if (n->activation > 0.6f && n->state.usefulness > 0.7f) {
            float score = n->activation * n->state.usefulness;
            if (score > best_score) {
                best_score = score;
                best_id = n->node_id;
            }
        }
    }
    return best_id;
}
