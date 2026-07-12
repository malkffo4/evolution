// runtime/ops/call.c
#include "runtime/vm/vm_status.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_types.h"
#include "runtime/operator/operator.h"
#include "runtime/logging/logging.h"

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
