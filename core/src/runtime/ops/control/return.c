// runtime/ops/control/return.c
#include <stdbool.h>
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/logging/logging.h"

int vm_op_return(VMContext *ctx, const Instruction *ins) {
    (void)ins;
    if (ctx->frame == 0) {
        ctx->halted = true;   // корневой уровень – останов
    } else {
        // Проматываем ip за границу кода, чтобы завершить цикл в vm_execute
        ctx->frames[ctx->frame].ip = ctx->frames[ctx->frame].pipeline->code_len;
        ctx->frame--;          // возвращаемся к вызывающему фрейму
    }
    return VM_OK;
}
