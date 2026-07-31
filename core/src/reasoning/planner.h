// core/src/reasoning/planner.h
#pragma once

#include <stdbool.h>
#include "types/id.h"
#include "memory/working.h"

typedef node_id_t instruction_t;

// Структура для удержания логической цепочки, вытащенной из БД
typedef struct {
    node_id_t start_node;
    node_id_t target_node;
    instruction_t* bytecode;
    size_t instruction_count;
} cognitive_chain_t;

void planner_evaluate_goals(WorkingMemory *wm, void *txn);
void set_goal_cooldown(uint64_t goal_id);
bool is_goal_on_cooldown(uint64_t goal_id);
