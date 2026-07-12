// runtime/ops/control/halt.c
#include <stdbool.h>
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_halt(VMContext *ctx, const Instruction *ins) {
    (void)ins;
    ctx->halted = true;
    return VM_OK;
}
