// runtime/ops/cognitive/concat_paths.c
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_concat_paths(VMContext *ctx, const Instruction *ins) {
    uint32_t sp_dest = ins->arg[0];
    uint32_t r1 = ins->arg[1];
    uint32_t r2 = ins->arg[2];
    uint32_t r3 = ins->arg[3];
    uint32_t r4 = ins->arg[4];

    if (r1 >= VM_MAX_REGISTERS || r2 >= VM_MAX_REGISTERS ||
        r3 >= VM_MAX_REGISTERS || r4 >= VM_MAX_REGISTERS ||
        sp_dest >= MAX_SCRATCHPAD)
        return VM_INVALID_REGISTER;

    node_id_t n1 = (node_id_t)ctx->reg[r1].i;
    node_id_t n2 = (node_id_t)ctx->reg[r2].i;
    node_id_t n3 = (node_id_t)ctx->reg[r3].i;
    node_id_t n4 = (node_id_t)ctx->reg[r4].i;

    // Освобождаем предыдущую строку, если была и это действительно указатель
    if (ctx->scratchpad[sp_dest].value && (uintptr_t)ctx->scratchpad[sp_dest].value > 0x1000) {
        free((void*)(uintptr_t)ctx->scratchpad[sp_dest].value);
    }

    char *path_str = malloc(128);
    if (!path_str) return VM_OUT_OF_MEMORY;
    snprintf(path_str, 128, "Path(%lu -> %lu ~> %lu -> %lu)",
             (unsigned long)n1, (unsigned long)n2,
             (unsigned long)n3, (unsigned long)n4);

    ctx->scratchpad[sp_dest].value = (int64_t)(uintptr_t)path_str;
    ctx->scratchpad[sp_dest].key_hash = 0;
    return VM_OK;
}
