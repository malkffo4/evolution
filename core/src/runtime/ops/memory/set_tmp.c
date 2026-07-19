// runtime/ops/memory/set_tmp.c
#include <stdint.h>
#include <string.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/vm/vm_param.h"

int vm_op_set_tmp(VMContext *ctx, const Instruction *ins) {
    uint32_t key_reg = ins->arg[0];
    uint32_t val_reg = ins->arg[1];

    if (key_reg >= VM_MAX_REGISTERS || val_reg >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    uint64_t key = ctx->reg[key_reg].i;   // предполагаем, что ключ – целое
    int64_t val  = ctx->reg[val_reg].i;

    // Простейший scratchpad в виде массива (можно заменить на хеш-таблицу)
    for (int i = 0; i < MAX_SCRATCHPAD; i++) {
        if (ctx->scratchpad[i].key_hash == 0) { // свободный слот
            ctx->scratchpad[i].key_hash = key;
            ctx->scratchpad[i].value = val;
            break;
        }
    }
    return VM_OK;
}
