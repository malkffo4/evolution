// runtime/vm/vm_pool.h
#pragma once

#include "runtime/compiler/pipeline.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "memory/working.h"

#define VM_POOL_MAX_WORKERS 32   // tune to core count; expose via config later

void vm_pool_init(void);
void vm_pool_shutdown(void);

// Воркеру больше не нужен чужой hmem, он сам создаст себе песочницу
void vm_pool_submit(Pipeline *pipeline, node_id_t goal_id, node_id_t algo_id);
