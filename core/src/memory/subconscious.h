// memory/subconscious.h

#ifndef SUBCONSCIOUS_H
#define SUBCONSCIOUS_H

#include <stdint.h>

#include "memory/working.h"

/* ---------------------- ОЧЕРЕДЬ ЗАДАЧ ---------------------- */
#define MAX_PENDING_TASKS 64

typedef struct {
    uint64_t node_id;       // Целевой узел, который нужно исследовать
    char query[256];        // Что искать (текст)
} ResearchTask;

void stop_subconscious_daemon(void);
void start_subconscious_daemon(WorkingMemory *wm);

#endif // SUBCONSCIOUS_H
