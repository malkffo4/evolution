// runtime/ops/cognitive/get_neighbors.c
#include <stdint.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

int vm_op_get_neighbors(VMContext *ctx, const Instruction *ins) {
    uint32_t src_reg = ins->arg[0];
    uint32_t rel_reg = ins->arg[1];
    uint32_t sp_start = ins->arg[2];
    uint32_t count_reg = ins->arg[3];

    if (src_reg >= VM_MAX_REGISTERS || rel_reg >= VM_MAX_REGISTERS ||
        count_reg >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    node_id_t src = (ctx->reg[src_reg].type == REG_NODE) ? ctx->reg[src_reg].node : (node_id_t)ctx->reg[src_reg].i;
    node_id_t rel = (ctx->reg[rel_reg].type == REG_NODE) ? ctx->reg[rel_reg].node : (node_id_t)ctx->reg[rel_reg].i;

    uint32_t count = 0;
    for (uint32_t i = 0; i < ctx->preloaded_edge_count; i++) {
        if (ctx->preloaded_edges[i].source == src &&
            ctx->preloaded_edges[i].relation == rel) {
            if (sp_start + count >= MAX_SCRATCHPAD) break;
            ctx->scratchpad[sp_start + count].key_hash = 0;
            ctx->scratchpad[sp_start + count].value = (int64_t)ctx->preloaded_edges[i].target;
            count++;
        }
    }

    ctx->reg[count_reg].type = REG_INT;
    ctx->reg[count_reg].i = count;
    return VM_OK;
}
