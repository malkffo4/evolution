// runtime/ops/control/branch.c
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_branch(VMContext *ctx, const Instruction *ins) {
    VMFrame *frame = &ctx->frames[ctx->frame];

    frame->ip = ins->arg[0];

    return VM_OK;
}

int vm_op_branch_if_empty(VMContext *ctx, const Instruction *ins) {
    uint32_t reg = ins->arg[0];
    uint32_t target = ins->arg[1];

    VMFrame *frame = &ctx->frames[ctx->frame];

    if (ctx->reg[reg].type == REG_EMPTY)
        frame->ip = target;

    return VM_OK;
}
