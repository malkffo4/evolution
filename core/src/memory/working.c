// core/src/memory/working.c
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "working.h"
#include "storage/db/db.h"
#include "math/hash.h"
#include "types/id.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "reasoning/algorithm_planner.h"
#include "reasoning/planner.h"
#include "ipc/ipc.h"

int wm_init(WorkingMemory *wm, uint32_t node_cap) {
    if (!wm) return 1;

    // Инициализация RW-Lock
    if (pthread_rwlock_init(&wm->lock, NULL) != 0) {
        return 1;
    }

    wm->capacity = node_cap > 0 ? node_cap : 100;
    wm->nodes = calloc(wm->capacity, sizeof(WorkingNode));
    if (!wm->nodes) {
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

    // 1. Ищем, есть ли уже этот узел в памяти
    for (uint32_t i = 0; i < wm->count; i++) {
        if (wm->nodes[i].node_id == node_id) {
            wm->nodes[i].activation += activation;
            if (wm->nodes[i].activation > 1.0f) wm->nodes[i].activation = 1.0f;
            pthread_rwlock_unlock(&wm->lock);
            return;
        }
    }

    // Вытеснение LRU (Least Recently Used / Coldest)
    if (wm->count >= wm->capacity) {
        uint32_t min_idx = 0;
        float min_act = wm->nodes[0].activation;

        // Ищем самый "холодный" узел
        for (uint32_t i = 1; i < wm->count; i++) {
            if (wm->nodes[i].activation < min_act) {
                min_act = wm->nodes[i].activation;
                min_idx = i;
            }
        }

        // Очищаем динамические свойства старого (вытесняемого) узла, чтобы не было утечки памяти
        DynamicProperty *curr = wm->nodes[min_idx].properties;
        while (curr) {
            DynamicProperty *next = curr->next;
            free(curr);
            curr = next;
        }

        // Заменяем вытесняемый узел последним элементом в массиве и уменьшаем счетчик
        wm->nodes[min_idx] = wm->nodes[wm->count - 1];
        wm->count--;
    }

    // 2. Добавляем новый узел
    wm->nodes[wm->count].node_id = node_id;
    wm->nodes[wm->count].activation = activation;
    wm->nodes[wm->count].focus_level = 0;
    wm->nodes[wm->count].state.novelty = base_emotion > 0 ? base_emotion : 0.5f;
    wm->nodes[wm->count].properties = NULL;
    wm->nodes[wm->count].attention_weight = base_emotion;
    wm->count++;

    // ФИКС: Освобождаем лок ПЕРЕД отправкой IPC события, чтобы избежать Deadlock
    pthread_rwlock_unlock(&wm->lock);

    // Транслируем событие о том, что узел попал в фокус мозга
    char event_buf[128];
    snprintf(event_buf, sizeof(event_buf), "{\"node_id\": %llu, \"activation\": %.2f}",
             (unsigned long long)node_id, activation);
    ipc_emit_event("NodeActivated", event_buf);
}

void wm_decay(WorkingMemory *wm) {
    if (!wm) return;
    pthread_rwlock_wrlock(&wm->lock);

    if (!wm->nodes) {
        pthread_rwlock_unlock(&wm->lock);
        return;
    }

    for (uint32_t i = 0; i < wm->count; i++) {
        wm->nodes[i].activation *= 0.9f;
        wm->nodes[i].state.urgency *= 0.8f;

        if (wm->nodes[i].activation < 0.05f) {
            DynamicProperty *curr = wm->nodes[i].properties;
            while (curr) {
                DynamicProperty *next = curr->next;
                free(curr);
                curr = next;
            }
            wm->nodes[i] = wm->nodes[wm->count - 1];
            wm->count--;
            i--;
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

void wm_rdlock(WorkingMemory *wm) {
    if (wm) pthread_rwlock_rdlock(&wm->lock);
}
void wm_wrlock(WorkingMemory *wm) {
    if (wm) pthread_rwlock_wrlock(&wm->lock);
}
void wm_unlock(WorkingMemory *wm) {
    if (wm) pthread_rwlock_unlock(&wm->lock);
}
