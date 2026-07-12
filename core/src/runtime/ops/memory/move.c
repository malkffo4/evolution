#include <stdint.h>
#include <string.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_move(VMContext *ctx, const Instruction *ins) {
    uint32_t dst = ins->arg[0];
    uint32_t src = ins->arg[1];

    if (dst >= VM_MAX_REGISTERS || src >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    ctx->reg[dst] = ctx->reg[src];

    return VM_OK;
}
