// memory/subconscious.h
#ifndef SUBCONSCIOUS_H
#define SUBCONSCIOUS_H

#include <stdint.h>

#include "storage/hyper_atom/hyper_atom.h"

typedef struct {
    uint64_t node_id;       // Целевой узел, который нужно исследовать
    char query[256];        // Что искать (текст)
} ResearchTask;

#define MAX_PENDING_TASKS 64

extern volatile int g_think_trigger;

void start_subconscious_daemon(void);
void stop_subconscious_daemon(void);

int get_pending_tasks(ResearchTask *buffer, int max_count);
void enqueue_research_task(uint64_t node_id, const char *query);

#endif // SUBCONSCIOUS_H
