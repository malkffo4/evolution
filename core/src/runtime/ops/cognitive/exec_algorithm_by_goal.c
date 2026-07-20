// runtime/ops/cognitive/exec_algorithm_by_goal.c
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/vm/vm.h"
#include "knowledge/algorithm_loader.h"

int vm_op_exec_algorithm_by_goal(VMContext *ctx, const Instruction *ins) {
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

    rc = vm_execute(ctx, algo_pipeline);

    if (algo_pipeline) {
        free(algo_pipeline->code);
        free(algo_pipeline);
    }
    return rc;
}
