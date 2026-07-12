// planner.c
#include <string.h>

#include "runtime/planner.h"
#include "runtime/vm/vm_context.h"
#include "runtime/capability/capability_types.h"
#include "runtime/operator/operator.h"

#define MAX_PER_CAP 64
typedef struct {
    CapabilityMask capability;
    Operator *ops[MAX_PER_CAP];
    int count;
} CapGroup;

static const Operator *default_policy_choose(VMContext *ctx, CapabilityMask cap);

static CapGroup groups[256];
static int group_count = 0;
static PlannerPolicy *active_policy = NULL;
static PlannerPolicy default_policy = { default_policy_choose };

void planner_init_default_policy(void) {
    active_policy = &default_policy;
}

void planner_set_policy(PlannerPolicy *policy) {
    active_policy = policy;
}

const Operator *planner_choose(VMContext *ctx, CapabilityMask cap) {
    if (active_policy && active_policy->choose)
        return active_policy->choose(ctx, cap);
    return NULL;
}

int planner_register_capability(Operator *op, CapabilityMask cap) {
    for (int i = 0; i < group_count; i++) {
        if (groups[i].capability == cap) {
            if (groups[i].count < MAX_PER_CAP) {
                groups[i].ops[groups[i].count++] = op;
                return 0;
            }
            return -1;
        }
    }
    if (group_count >= 256) return -1;
    groups[group_count].capability = cap;
    groups[group_count].ops[0] = op;
    groups[group_count].count = 1;
    group_count++;
    return 0;
}

static const Operator *default_policy_choose(VMContext *ctx, CapabilityMask cap) {
    for (int i = 0; i < group_count; i++) {
        if (groups[i].capability == cap && groups[i].count > 0) {
            const Operator *best = NULL;
            double best_score = 1e30;
            for (int j = 0; j < groups[i].count; j++) {
                Operator *op = groups[i].ops[j];
                VMProfile *p = &ctx->profile[op->id];
                double avg_cycles = p->calls ? (double)p->cycles / p->calls : 1000.0;
                double penalty = p->failures * 500.0;
                double score = avg_cycles + penalty;
                if (score < best_score) {
                    best_score = score;
                    best = op;
                }
            }
            return best;
        }
    }
    return NULL;
}
