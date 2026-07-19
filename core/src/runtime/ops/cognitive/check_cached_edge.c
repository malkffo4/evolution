// runtime/ops/cognitive/check_cached_edge.c
#include <stdint.h>
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"

static inline node_id_t reg_as_node(const Register *r) {
    if (r->type == REG_NODE) return r->node;
    if (r->type == REG_INT)  return (node_id_t)r->i;
    return 0;  // недопустимо, проверка типов выше
}

int vm_op_check_cached_edge(VMContext *ctx, const Instruction *ins) {
    uint32_t dst = ins->arg[0];
    uint32_t src = ins->arg[1];
    uint32_t rel = ins->arg[2];
    uint32_t tgt = ins->arg[3];

    if (dst >= VM_MAX_REGISTERS || src >= VM_MAX_REGISTERS ||
        rel >= VM_MAX_REGISTERS || tgt >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    if ((ctx->reg[src].type != REG_NODE && ctx->reg[src].type != REG_INT) ||
        (ctx->reg[rel].type != REG_NODE && ctx->reg[rel].type != REG_INT) ||
        (ctx->reg[tgt].type != REG_NODE && ctx->reg[tgt].type != REG_INT))
        return VM_INVALID_TYPE;

    node_id_t source   = reg_as_node(&ctx->reg[src]);
    node_id_t relation = reg_as_node(&ctx->reg[rel]);
    node_id_t target   = reg_as_node(&ctx->reg[tgt]);

    bool found = false;
    for (uint32_t i = 0; i < ctx->preloaded_edge_count; i++) {
        if (ctx->preloaded_edges[i].source == source &&
            ctx->preloaded_edges[i].relation == relation &&
            ctx->preloaded_edges[i].target == target) {
            found = true;
            break;
        }
    }

    ctx->reg[dst].type = REG_BOOL;
    ctx->reg[dst].b = found;
    return VM_OK;
}
