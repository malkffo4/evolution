// core/src/memory/working.c
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "working.h"
#include "storage/db/db.h"
#include "math/hash.h"
#include "storage/node/node.h"
#include "storage/edge/edge.h"
#include "types/id.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "reasoning/algorithm_planner.h"
#include "reasoning/planner.h"

int wm_init(WorkingMemory *wm, uint32_t node_cap, uint32_t edge_cap) {
    if (!wm) return 1;

    // Инициализация RW-Lock
    if (pthread_rwlock_init(&wm->lock, NULL) != 0) {
        return 1;
    }

    wm->active_nodes.items = malloc(node_cap * sizeof(node_id_t));
    if (!wm->active_nodes.items) {
        pthread_rwlock_destroy(&wm->lock);
        return 1;
    }
    wm->active_nodes.count = 0;
    wm->active_nodes.capacity = node_cap;

    wm->active_edges.items = malloc(edge_cap * sizeof(Edge));
    if (!wm->active_edges.items) {
        free(wm->active_nodes.items);
        pthread_rwlock_destroy(&wm->lock);
        return 1;
    }
    wm->active_edges.count = 0;
    wm->active_edges.capacity = edge_cap;

    wm->capacity = 100; // Лимит, используемый в wm_activate
    wm->nodes = calloc(wm->capacity, sizeof(WorkingNode));
    if (!wm->nodes) {
        free(wm->active_edges.items);
        free(wm->active_nodes.items);
        pthread_rwlock_destroy(&wm->lock);
        return 1;
    }
    wm->count = 0;
    wm->tick = 0;
    
    return 0;
}

void wm_clear(WorkingMemory *wm) {
    if (!wm) return;
    pthread_rwlock_wrlock(&wm->lock);
    if (wm->active_nodes.items) {
        free(wm->active_nodes.items);
        wm->active_nodes.items = NULL;
    }
    if (wm->active_edges.items) {
        free(wm->active_edges.items);
        wm->active_edges.items = NULL;
    }
    if (wm->nodes) {
        for (uint32_t i = 0; i < wm->count; i++) {
            DynamicProperty *curr = wm->nodes[i].properties;
            while (curr) {
                DynamicProperty *next = curr->next;
                free(curr);
                curr = next;
            }
        }
        free(wm->nodes);
        wm->nodes = NULL;
    }
    wm->active_nodes.count = 0;
    wm->active_edges.count = 0;
    wm->count = 0;
    pthread_rwlock_unlock(&wm->lock);
    pthread_rwlock_destroy(&wm->lock);
}

void wm_activate(WorkingMemory *wm, uint64_t node_id, float activation, float base_emotion) {
    if (!wm) return;
    pthread_rwlock_wrlock(&wm->lock);
    if (!wm->nodes) {
        pthread_rwlock_unlock(&wm->lock);
        return;
    }

    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].node_id == node_id) {
            wm->nodes[i].activation += activation;
            if (wm->nodes[i].activation > 1.0f) wm->nodes[i].activation = 1.0f;
            pthread_rwlock_unlock(&wm->lock);
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
    pthread_rwlock_unlock(&wm->lock);
}

void wm_decay(WorkingMemory *wm) {
    if (!wm) return;
    pthread_rwlock_wrlock(&wm->lock);
    if (!wm->nodes) {
        pthread_rwlock_unlock(&wm->lock);
        return;
    }

    for (uint32_t i = 0; i < wm->count; i++) {
        wm->nodes[i].activation *= 0.9f; // Угасание мысли на 10% каждый тик
        wm->nodes[i].state.urgency *= 0.8f;

        // Если узел остыл - освобождаем динамические свойства и вычищаем из массива
        if (wm->nodes[i].activation < 0.05f) {
            DynamicProperty *curr = wm->nodes[i].properties;
            while (curr) {
                DynamicProperty *next = curr->next;
                free(curr);
                curr = next;
            }
            wm->nodes[i] = wm->nodes[wm->count - 1];
            wm->count--;
            i--; // Повторяем проверку для сдвинутого элемента
        }
    }
    pthread_rwlock_unlock(&wm->lock);
}

void engine_spread_activation(WorkingMemory *wm, void *lmdb_txn) {
    (void)lmdb_txn;
    if (!wm) return;
    pthread_rwlock_wrlock(&wm->lock);
    if (!wm->nodes || wm->count < 2) {
        pthread_rwlock_unlock(&wm->lock);
        return;
    }

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
    pthread_rwlock_unlock(&wm->lock);
}

void wm_set_property(WorkingMemory *wm, uint64_t node_id, const char *key, const char *value) {
    if (!wm || !key || !value) return;
    pthread_rwlock_wrlock(&wm->lock);
    if (!wm->nodes) {
        pthread_rwlock_unlock(&wm->lock);
        return;
    }

    uint64_t key_hash = djb2_hash(key);
    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].node_id == node_id) {
            DynamicProperty *curr = wm->nodes[i].properties;
            while (curr) {
                if (curr->key_hash == key_hash) {
                    strncpy(curr->value, value, sizeof(curr->value) - 1);
                    curr->value[sizeof(curr->value) - 1] = '\0';
                    pthread_rwlock_unlock(&wm->lock);
                    return;
                }
                curr = curr->next;
            }

            DynamicProperty *new_prop = calloc(1, sizeof(DynamicProperty));
            if (!new_prop) {
                pthread_rwlock_unlock(&wm->lock);
                return;
            }
            new_prop->key_hash = key_hash;
            strncpy(new_prop->value, value, sizeof(new_prop->value) - 1);
            new_prop->value[sizeof(new_prop->value) - 1] = '\0';
            new_prop->next = wm->nodes[i].properties;
            wm->nodes[i].properties = new_prop;
            
            pthread_rwlock_unlock(&wm->lock);
            return;
        }
    }
    pthread_rwlock_unlock(&wm->lock);
}

const char* wm_get_property(WorkingMemory *wm, uint64_t node_id, const char *key) {
    if (!wm || !key) return NULL;
    pthread_rwlock_rdlock(&wm->lock);
    if (!wm->nodes) {
        pthread_rwlock_unlock(&wm->lock);
        return NULL;
    }

    uint64_t key_hash = djb2_hash(key);
    const char *result = NULL;
    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].node_id == node_id) {
            DynamicProperty *curr = wm->nodes[i].properties;
            while (curr) {
                if (curr->key_hash == key_hash) {
                    result = curr->value;
                    break;
                }
                curr = curr->next;
            }
            break;
        }
    }
    pthread_rwlock_unlock(&wm->lock);
    return result;
}

node_id_t wm_get_highest_goal(WorkingMemory *wm, HyperMemory *hmem, float activation_threshold) {
    if (!wm) return 0;

    pthread_rwlock_rdlock(&wm->lock);
    if (!wm->nodes) {
        pthread_rwlock_unlock(&wm->lock);
        return 0;
    }

    node_id_t best_id = 0;
    float best_score = -1.0f;

    for (uint32_t i = 0; i < wm->count; i++) {
        WorkingNode *n = &wm->nodes[i];
        // Единый гомеостатический порог вместо раздельных хардкодов 0.6f/0.7f.
        // usefulness должен быть чуть строже activation — коэффициент 1.15
        // отражает то, что "полезность" исторически была более консервативным
        // фильтром в исходном коде; сохраняем это отношение, но привязываем
        // оба к одному дрейфующему параметру.
        if (n->activation < activation_threshold ||
            n->state.usefulness < activation_threshold * 1.15f)
            continue;
        if (is_goal_on_cooldown(n->node_id)) continue;

        // Динамическая проверка: есть ли атом отношения Goal-Algorithm?
        node_id_t rel_ids[16];
        size_t rel_count = find_goal_algorithm_relations(hmem, rel_ids, 16);

        bool is_goal = false;
        for (size_t r = 0; r < rel_count; r++) {
            NeuroAtom *atoms = NULL;
            size_t count = 0;
            if (hyper_find_by_process(hmem, rel_ids[r], n->node_id, 0, &atoms, &count) == 0 && count > 0) {
                free(atoms);
                is_goal = true;
                break;
            }
            if (atoms) free(atoms);
        }

        if (!is_goal) {
            continue;
        }

        float score = n->activation * n->state.usefulness;
        if (score > best_score) {
            best_score = score;
            best_id = n->node_id;
        }
    }

    pthread_rwlock_unlock(&wm->lock);
    return best_id;
}

// Потокобезопасные обёртки для ручной блокировки из VM-операторов при необходимости
void wm_rdlock(WorkingMemory *wm) {
    if (wm) pthread_rwlock_rdlock(&wm->lock);
}

void wm_wrlock(WorkingMemory *wm) {
    if (wm) pthread_rwlock_wrlock(&wm->lock);
}

void wm_unlock(WorkingMemory *wm) {
    if (wm) pthread_rwlock_unlock(&wm->lock);
}