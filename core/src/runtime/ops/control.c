// runtime/ops/control.c
#include <stdbool.h>

#include "runtime/vm/vm_status.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_types.h"
#include "runtime/operator/operator.h"
#include "runtime/logging/logging.h"

int vm_op_branch(VMContext *ctx, const Instruction *ins) {
    VMFrame *frame = &ctx->frames[ctx->frame];

    frame->ip = ins->arg[0];

    return VM_OK;
}

int vm_op_branch_if_empty(VMContext *ctx, const Instruction *ins) {
    uint32_t reg = ins->arg[0];
    uint32_t target = ins->arg[1];

    VMFrame *frame = &ctx->frames[ctx->frame];

    if (ctx->reg[reg].type == REG_EMPTY)
        frame->ip = target;

    return VM_OK;
}

// Здесь нужно загрузить новый код из контекста, но пока игнорируем
// Так делать нельзя. Когда VM вызывает под-пайплайн (например, `FindAnalogy` внутри `Reasoning`),
// новый фрейм должен получать свой собственный `Instruction *code` и сбрасывать `ip` в 0,
// но **сохранять ссылку на предыдущий фрейм** (или номер текущего регистра),
// чтобы при опкоде `OP_RETURN` вернуть результат в вызывающий пайплайн.
// У тебя сейчас фреймы не имеют механизма передачи `return value` обратно в регистры родителя.
int vm_op_call(VMContext *ctx, const Instruction *ins) {
    OperatorID id = (OperatorID)ins->arg[0];
    const Operator *op = operator_find(id);

    if (!op) {
        LOG_ERROR("VM: OP_CALL received %d", id);
        return VM_ERROR;
    }

    return operator_execute(ctx, op, ins);
}

// TODO архитектурная проблема
// Вот эта инструкция CALL у тебя сейчас хранит Instruction.operator
// То есть она всегда вызывает один и тот же Operator.
// Это означает CALL FindEdges нормально. Но вот CALL register0 уже невозможно.
// То есть сейчас CALL — не "динамический вызов", а "статический переход".
// Для первой версии VM это нормально.

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
