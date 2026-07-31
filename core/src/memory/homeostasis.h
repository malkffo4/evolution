// memory/homeostasis.h
#pragma once

#include <stdint.h>
#include "memory/decay.h"
#include "memory/working.h"

typedef struct {
    float    target_load;      // L*, целевая заполненность WM (0..1)
    float    eta_threshold;    // скорость адаптации порога активации целей
    float    eta_decay;        // скорость адаптации коэффициента затухания STI
    float    threshold_min;
    float    threshold_max;
    float    decay_floor;
    uint64_t sweep_seconds;    // желаемый период полного обхода архива decay'ем
    uint64_t tick_seconds;     // период вызова decay-цикла (см. subconscious.c)
} HomeostasisConfig;

extern const HomeostasisConfig HOMEOSTASIS_DEFAULT;

typedef struct {
    float       activation_threshold; // заменяет хардкод 0.6f/0.7f в working.c
    DecayPolicy policy;                // заменяет DECAY_POLICY_DEFAULT
} HomeostasisState;

void homeostasis_init(HomeostasisState *st);

// total_atom_count берётся вызывающей стороной через mdb_stat(db.graph.hyper.atoms).
void homeostasis_step(HomeostasisState *st, const HomeostasisConfig *cfg,
                       WorkingMemory *wm, uint64_t total_atom_count);