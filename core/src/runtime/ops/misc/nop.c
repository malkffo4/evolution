#include "runtime/vm/vm.h"
#include "runtime/ops/vm_ops.h"

int vm_op_nop(VMContext *ctx, const Instruction *ins) {
    (void)ctx; (void)ins;
    return VM_OK;
}
