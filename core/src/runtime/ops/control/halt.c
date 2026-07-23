// runtime/ops/control/halt.c
#include <stdbool.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_halt(VMContext *ctx, const Instruction *ins) {
    (void)ins;
    // Если мы в корневом фрейме – действительно останавливаем VM.
    // Иначе просто выходим из текущей подпрограммы.
    if (ctx->frame == 0) {
        ctx->halted = true; // останавливаем всю VM
    } else {
        // завершаем текущий фрейм: перематываем ip за границу кода
        ctx->frames[ctx->frame].ip = ctx->frames[ctx->frame].pipeline->code_len;
    }
    return VM_OK;
}
