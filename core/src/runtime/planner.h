// planner.h
#ifndef PLANNER_H
#define PLANNER_H

#include "runtime/vm/vm_context.h"
#include "runtime/capability/capability_types.h"
#include "runtime/operator/operator.h"

typedef struct PlannerPolicy {
    const Operator *(*choose)(VMContext *ctx, CapabilityMask cap); // , const Operator **result
} PlannerPolicy;

void planner_set_policy(PlannerPolicy *policy);
const Operator *planner_choose(VMContext *ctx, CapabilityMask cap);
int planner_register_capability(Operator *op, CapabilityMask cap);
void planner_init_default_policy(void);

#endif // PLANNER_H
