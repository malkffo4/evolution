// runtime/ops/cognitive/exec_algorithm.c
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/vm/vm.h"          // vm_execute
#include "knowledge/algorithm_loader.h"

int vm_op_exec_algorithm(VMContext *ctx, const Instruction *ins) {
    uint32_t reg_idx = ins->arg[0];
    if (reg_idx >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    node_id_t algo_id;
    if (ctx->reg[reg_idx].type == REG_NODE)
        algo_id = ctx->reg[reg_idx].node;
    else if (ctx->reg[reg_idx].type == REG_INT)
        algo_id = (node_id_t)ctx->reg[reg_idx].i;
    else
        return VM_INVALID_TYPE;

    Pipeline *algo_pipeline = NULL;
    int rc = algorithm_load(ctx->memory.txn, algo_id, &algo_pipeline);
    if (rc != 0)
        return VM_NOT_FOUND;

    // Защита от переполнения стека
    if (ctx->frame + 1 >= VM_MAX_CALL_DEPTH) {
        free(algo_pipeline->code);
        free(algo_pipeline);
        return VM_STACK_OVERFLOW;
    }

    // Сохраняем состояние
    uint32_t prev_frame = ctx->frame;
    bool prev_halted = ctx->halted;

    // Переключаемся на новый фрейм
    ctx->frame++;
    VMFrame *f = &ctx->frames[ctx->frame];
    f->pipeline = algo_pipeline;
    f->code     = algo_pipeline->code;
    f->ip       = 0;

    ctx->halted = false;   // сбрасываем halted для подпрограммы
    rc = vm_execute(ctx, algo_pipeline);

    // Восстанавливаем состояние
    ctx->frame = prev_frame;
    ctx->halted = prev_halted;

    // Освобождаем загруженный pipeline
    free(algo_pipeline->code);
    free(algo_pipeline);
    return rc;
}
