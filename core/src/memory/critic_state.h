// memory/critic_state.h
#ifndef CRITIC_STATE_H
#define CRITIC_STATE_H

#include <stdint.h>
#include <stdbool.h>

void init_quarantine(void);

bool is_quarantined(uint64_t algo_id);

void record_execution_result(uint64_t algo_id, int rc);

#endif // CRITIC_STATE_H
