// memory/working.h
#ifndef WORKING_H
#define WORKING_H

#include <stdint.h>

#include "types/id.h"
#include "memory/cognitive.h"
#include "storage/graph/graph.h"

// Динамическое свойство узла (ключ-значение)
typedef struct DynamicProperty {
    uint64_t key_hash;
    char value[256];
    struct DynamicProperty *next;
} DynamicProperty;

// Динамическое состояние узла в оперативной памяти (Working Memory)
typedef struct {
    uint64_t node_id;
    float activation;      // Текущая "заряженность" темы (0.0 - 1.0)
    uint32_t last_seen_tick;
    CognitiveValue state;
    float attention_weight;
    float prediction_error;
    float reward;
    float inhibition;
    uint8_t focus_level;
    DynamicProperty *properties;  // Динамические свойства узла
} WorkingNode;

typedef struct {
    NodeList  active_nodes;
    EdgeList  active_edges;
    // … дополнительные поля
    WorkingNode *nodes;
    uint32_t count;
    uint32_t capacity;
    uint64_t tick;
} WorkingMemory;

// Интерфейсы модулей
int wm_init(WorkingMemory *wm, uint32_t node_cap, uint32_t edge_cap);
void wm_clear(WorkingMemory *wm);
void wm_activate(WorkingMemory *wm, uint64_t node_id, float energy, float exploit_potential);
void wm_decay(WorkingMemory *wm);

void engine_spread_activation(WorkingMemory *wm, void *lmdb_txn);

// TODO ???
// memory_tick();
// memory_activate();
// memory_decay();
// memory_consolidate();

#endif
