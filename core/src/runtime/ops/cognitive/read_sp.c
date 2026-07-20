// runtime/ops/cognitive/read_sp.c
#include <stdint.h>
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_read_sp(VMContext *ctx, const Instruction *ins) {
    uint32_t dst_reg = ins->arg[0];
    uint32_t sp_idx  = ins->arg[1];

    if (dst_reg >= VM_MAX_REGISTERS || sp_idx >= MAX_SCRATCHPAD)
        return VM_INVALID_REGISTER;

    ctx->reg[dst_reg].type = REG_INT;          // scratchpad хранит int64_t
    ctx->reg[dst_reg].i = ctx->scratchpad[sp_idx].value;

    return VM_OK;
}
