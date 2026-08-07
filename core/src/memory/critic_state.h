// memory/critic_state.h
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAX_QUARANTINE_NODES            64
#define QUARANTINE_BASE_COOLDOWN_SEC    60 // 1 минута отдыха для зациклившегося алгоритма

typedef struct {
    uint64_t algo_id;
    int consecutive_failures;
    time_t quarantined_until;
} QuarantineEntry;

typedef struct {
    uint64_t algo_id;
    int consecutive_failures;
} FailureSnapshot;

uint32_t critic_dump_failures(FailureSnapshot *out, int max);

void init_quarantine(void);

bool is_quarantined(uint64_t algo_id);

void record_execution_result(uint64_t algo_id, int rc);
