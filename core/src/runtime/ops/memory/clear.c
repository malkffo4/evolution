// runtime/ops/memory/clear.c
#include <stdint.h>
#include <string.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_clear(VMContext *ctx, const Instruction *ins) {
    uint32_t reg = ins->arg[0];

    if (reg >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    memset(&ctx->reg[reg], 0, sizeof(Register));
    ctx->reg[reg].type = REG_EMPTY;

    return VM_OK;
}
