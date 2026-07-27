// runtime/ops/cognitive/get_edge.c
#include <stdint.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "storage/db/db.h"
#include "storage/edge/edge.h"

int vm_op_get_edge(VMContext *ctx, const Instruction *ins) {
    uint32_t dst   = ins->arg[0];
    uint32_t src   = ins->arg[1];
    uint32_t rel   = ins->arg[2];
    uint32_t tgt   = ins->arg[3];

    if (dst >= VM_MAX_REGISTERS || src >= VM_MAX_REGISTERS ||
        rel >= VM_MAX_REGISTERS || tgt >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    if (ctx->reg[src].type != REG_NODE || ctx->reg[rel].type != REG_NODE ||
        ctx->reg[tgt].type != REG_NODE)
        return VM_INVALID_TYPE;

    node_id_t source   = ctx->reg[src].node;
    node_id_t relation = ctx->reg[rel].node;
    node_id_t target   = ctx->reg[tgt].node;

    MDB_txn *txn = ctx->memory.txn;
    Triple key = { source, relation, target };
    Edge edge;
    int rc = get_edge(txn, &key, &edge);

    ctx->reg[dst].type = REG_BOOL;
    ctx->reg[dst].b = (rc == MDB_SUCCESS);

    return VM_OK;
}
