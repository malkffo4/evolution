// runtime/ops/cognitive/legacy_bridge.c
#include "runtime/vm/vm_context.h"
#include "memory/working.h"
#include "reasoning/planner.h"

int vm_op_spread_activation(VMContext *ctx, const Instruction *ins) {
    (void)ins;
    if (ctx->memory.wm && ctx->memory.txn)
        engine_spread_activation(ctx->memory.wm, ctx->memory.txn);
    return VM_OK;
}

int vm_op_evaluate_goals(VMContext *ctx, const Instruction *ins) {
    (void)ins;
    if (ctx->memory.wm && ctx->memory.txn)
        planner_evaluate_goals(ctx->memory.wm, ctx->memory.txn);
    return VM_OK;
}
