// runtime/ops/memory/load_const.c
#include <stdint.h>

// #include "runtime/compiler/pipeline.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_load_const(VMContext *ctx, const Instruction *ins) {
    uint32_t dst = ins->arg[0];
    if (dst >= VM_MAX_REGISTERS) return VM_INVALID_REGISTER;
    ctx->reg[dst].type = REG_INT;
    ctx->reg[dst].i = (int64_t)(uint32_t)ins->arg[1];
    return VM_OK;
}
