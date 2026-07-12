#include <stdint.h>

// #include "runtime/compiler/pipeline.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_load_const(VMContext *ctx, const Instruction *ins) {
    uint32_t dst       = ins->arg[0];
    uint32_t const_idx = ins->arg[1];

    VMFrame *frame = &ctx->frames[ctx->frame];
    const Pipeline *pl = frame->pipeline;

    if (!pl)
        return VM_ERROR;

    if (const_idx >= pl->constants.int_count)
        return VM_ERROR;

    if (dst >= VM_MAX_REGISTERS)
        return VM_ERROR;

    ctx->reg[dst].type = REG_INT;
    ctx->reg[dst].i = pl->constants.int_consts[const_idx];

    return VM_OK;
}
