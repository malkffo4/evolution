// src/memory/subconscious.h
#ifndef SUBCONSCIOUS_H
#define SUBCONSCIOUS_H

#include <stdint.h>

#include "storage/hyper_atom/hyper_atom.h"
#include "memory/working.h"

typedef struct {
    uint64_t node_id;       // Целевой узел, который нужно исследовать
    char query[256];        // Что искать (текст)
} ResearchTask;

#define MAX_PENDING_TASKS 64

#define MAIN_LOOP_ALGO_ID  0x4D41494E4C4F4F50ULL  // "MAINLOOP" в little-endian?
// Лучше использовать хэш: djb2_hash("MainLoop") -> добавим в код инициализации

void start_subconscious_daemon(WorkingMemory *wm, HyperMemory *hmem);
void stop_subconscious_daemon(void);

#endif
