// runtime/ops/memory_ops.c
#include <stdint.h>
#include <string.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/vm/vm_param.h"

int vm_op_add(VMContext *ctx, const Instruction *ins) {
    uint32_t dst  = ins->arg[0];
    uint32_t src1 = ins->arg[1];
    uint32_t src2 = ins->arg[2];

    if (dst >= VM_MAX_REGISTERS || src1 >= VM_MAX_REGISTERS || src2 >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    if (ctx->reg[src1].type != REG_INT || ctx->reg[src2].type != REG_INT)
        return VM_INVALID_TYPE;

    ctx->reg[dst].type = REG_INT;
    ctx->reg[dst].i = ctx->reg[src1].i + ctx->reg[src2].i;

    return VM_OK;
}

int vm_op_clear(VMContext *ctx, const Instruction *ins) {
    uint32_t reg = ins->arg[0];

    if (reg >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    memset(&ctx->reg[reg], 0, sizeof(Register));
    ctx->reg[reg].type = REG_EMPTY;

    return VM_OK;
}

int vm_op_load_const(VMContext *ctx, const Instruction *ins) {
    uint32_t dst       = ins->arg[0];
    uint32_t const_idx = ins->arg[1];
    VMFrame *frame = &ctx->frames[ctx->frame];
    const Pipeline *pl = frame->pipeline;
    int64_t value = 0;

    if (!pl)
        return VM_ERROR;

    if (dst >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    /* Если есть пул констант – берём значение из пула */
    if (pl->constants.int_consts && const_idx < pl->constants.int_count) {
        value = pl->constants.int_consts[const_idx];
    } else {
        /* Иначе используем непосредственное значение (32 бита) */
        value = (int64_t)(uint32_t)ins->arg[1];
    }

    ctx->reg[dst].type = REG_INT;
    ctx->reg[dst].i = value;
    return VM_OK;
}

int vm_op_move(VMContext *ctx, const Instruction *ins) {
    uint32_t dst = ins->arg[0];
    uint32_t src = ins->arg[1];

    if (dst >= VM_MAX_REGISTERS || src >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    ctx->reg[dst] = ctx->reg[src];

    return VM_OK;
}

int vm_op_set_tmp(VMContext *ctx, const Instruction *ins) {
    uint32_t key_reg = ins->arg[0];
    uint32_t val_reg = ins->arg[1];

    if (key_reg >= VM_MAX_REGISTERS || val_reg >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    uint64_t key = ctx->reg[key_reg].ui;   // предполагаем, что ключ – целое
    int64_t val  = ctx->reg[val_reg].i;

    // Простейший scratchpad в виде массива (можно заменить на хеш-таблицу)
    for (uint32_t i = 0; i < MAX_SCRATCHPAD; i++) {
        if (ctx->scratchpad[i].key_hash == 0) { // свободный слот
            ctx->scratchpad[i].key_hash = key;
            ctx->scratchpad[i].value = val;
            break;
        }
    }
    return VM_OK;
}

int vm_op_store(VMContext *ctx, const Instruction *ins) {
    // заглушка: сохранение результата в рабочую память
    (void)ctx; (void)ins;
    return VM_OK;
}
