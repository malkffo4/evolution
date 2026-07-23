// runtime/ops/memory/store.c
// #include <string.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_store(VMContext *ctx, const Instruction *ins) {
    // заглушка: сохранение результата в рабочую память
    (void)ctx; (void)ins;
    return VM_OK;
}
