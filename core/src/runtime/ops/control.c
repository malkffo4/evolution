// runtime/ops/control.c
#include <stdbool.h>

#include "runtime/vm/vm_status.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_types.h"
#include "runtime/operator/operator.h"
#include "runtime/logging/logging.h"
#include "knowledge/algorithm_loader.h"

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

int vm_op_call(VMContext *ctx, const Instruction *ins) {
    // В продвинутой VM arg[0] может быть либо ID встроенного C-оператора,
    // либо ID пайплайна (ko_id_t) в базе знаний.
    // Если флаг указывает на вызов пайплайна:
    uint64_t target_algo_id = ins->arg[0];

    // 1. Проверяем переполнение стека фреймов
    if (ctx->frame + 1 >= VM_MAX_CALL_DEPTH) {
        LOG_ERROR("VM: Call stack overflow");
        return VM_ERROR;
    }

    // 2. Загружаем пайплайн из базы знаний (требует доступ к MDB_txn)
    Pipeline *new_pipeline = NULL;
    if (algorithm_load(ctx->memory.txn, target_algo_id, &new_pipeline) != 0 || !new_pipeline) {
        LOG_ERROR("VM: Pipeline %llu not found for OP_CALL", (unsigned long long)target_algo_id);
        return VM_NOT_FOUND;
    }

    // 3. Создаем новый фрейм
    ctx->frame++;
    VMFrame *new_frame = &ctx->frames[ctx->frame];
    new_frame->pipeline = new_pipeline;
    new_frame->code = new_pipeline->code;
    new_frame->ip = 0;

    // Передача аргументов (arg[1], arg[2]) в регистры нового фрейма
    // может быть реализована здесь, если мы добавим смещение регистров (Register Windows)
    // или стек. Пока работаем с глобальными регистрами контекста.

    return VM_OK;
}

int vm_op_halt(VMContext *ctx, const Instruction *ins) {
    (void)ins;
    ctx->halted = true; // Безусловная полная остановка всей VM
    return VM_OK;
}

int vm_op_return(VMContext *ctx, const Instruction *ins) {
    (void)ins;

    // Освобождаем память пайплайна текущего фрейма, если мы его загрузили динамически
    // Примечание: Убедись, что MainLoop не освобождается дважды,
    // для этого можно использовать флаг is_dynamic в структуре VMFrame.
    if (ctx->frame > 0) {
        if (ctx->frames[ctx->frame].pipeline) {
            pipeline_free((Pipeline*)ctx->frames[ctx->frame].pipeline);
        }

        ctx->frame--; // Возвращаемся к предыдущему фрейму

        // ip предыдущего фрейма уже указывает на следующую инструкцию
        // после OP_CALL, так как он инкрементировался перед вызовом
    } else {
        // Мы в корневом фрейме — возвращаться некуда
        ctx->halted = true;
    }
    return VM_OK;
}
