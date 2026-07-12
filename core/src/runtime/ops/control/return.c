// runtime/ops/control/return.c
#include <stdbool.h>
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/logging/logging.h"

int vm_op_return(VMContext *ctx, const Instruction *ins) {
    (void)ins; // Аргумент возврата пока не используем, регистры общие

    if (ctx->frame == 0) {
        // Мы в корневом фрейме - завершаем работу VM
        ctx->halted = true;
        LOG_DEBUG("VM: HALTED gracefully.");
        return VM_OK;
    }

    // Возвращаемся в вызывающий пайплайн (POP FRAME)
    LOG_DEBUG("VM: POP FRAME -> Leaving depth %d", ctx->frame);
    ctx->frame--;

    return VM_OK;
}
