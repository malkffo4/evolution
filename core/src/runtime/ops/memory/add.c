#include <stdint.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_add(VMContext *ctx, const Instruction *ins) {
    uint32_t dst  = ins->arg[0];
    uint32_t src1 = ins->arg[1];
    uint32_t src2 = ins->arg[2];

    if (dst >= VM_MAX_REGISTERS || src1 >= VM_MAX_REGISTERS || src2 >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    if (ctx->reg[src1].type != REG_INT || ctx->reg[src2].type != REG_INT)
        return VM_INVALID_TYPE;

    ctx->reg[dst].type = REG_INT;
    ctx->reg[dst].i = ctx->reg[src1].i + ctx->reg[src2].i;

    return VM_OK;
}
