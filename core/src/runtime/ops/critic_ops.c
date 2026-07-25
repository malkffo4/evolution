// runtime/ops/critic_ops.c
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "memory/critic_state.h" // добавить getter в critic_state.c

// В critic_state.c нужно добавить:
// int critic_dump_failures(FailureSnapshot *out, int max) — экспорт
// consecutive_failures по всем algo_id, не только bool is_quarantined.

int vm_op_read_failures(VMContext *ctx, const Instruction *ins) {
    uint32_t sp_offset = ins->arg[0];
    uint32_t count_reg = ins->arg[1];

    FailureSnapshot snap[MAX_QUARANTINE_NODES];
    int n = critic_dump_failures(snap, MAX_QUARANTINE_NODES);

    for (int i = 0; i < n && (sp_offset + i * 2 + 1) < MAX_SCRATCHPAD; i++) {
        ctx->scratchpad[sp_offset + i*2].value     = (int64_t)snap[i].algo_id;
        ctx->scratchpad[sp_offset + i*2 + 1].value = (int64_t)snap[i].consecutive_failures;
    }

    ctx->reg[count_reg].type = REG_INT;
    ctx->reg[count_reg].i = n;
    return VM_OK;
}
