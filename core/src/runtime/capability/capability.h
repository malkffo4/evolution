// runtime/capability.h
#ifndef CAPABILITY_H
#define CAPABILITY_H

#include <stdint.h>

#include "runtime/operator/operator_types.h"
#include "runtime/capability/capability_types.h"

typedef struct {
    CapabilityID id;
    const char *name;
} Capability;

typedef struct {
    OperatorID operator;
    uint64_t calls;
    double accuracy;
    double latency;
    double confidence;
    double reward;
} CapabilityCandidate;

#endif // CAPABILITY_H
