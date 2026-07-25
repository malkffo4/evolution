// runtime/ops/critic_ops.c
#include <stdint.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "memory/critic_state.h" // добавить getter в critic_state.c

typedef struct FailureSnapshot {
    uint64_t algo_id;
    uint64_t consecutive_failures;
} FailureSnapshot;

// В critic_state.c нужно добавить:
// — экспорт
uint32_t critic_dump_failures(FailureSnapshot *out, int max) {
    (void)out;
    max = 0;

    return 0;
}

// consecutive_failures по всем algo_id, не только bool is_quarantined.
int vm_op_read_failures(VMContext *ctx, const Instruction *ins) {
    uint32_t sp_offset = ins->arg[0];
    uint32_t count_reg = ins->arg[1];

    FailureSnapshot snap[MAX_QUARANTINE_NODES];
    uint32_t n = critic_dump_failures(snap, MAX_QUARANTINE_NODES);

    for (uint32_t i = 0; i < n && (sp_offset + i * 2 + 1) < MAX_SCRATCHPAD; i++) {
        ctx->scratchpad[sp_offset + i*2].value     = (int64_t)snap[i].algo_id;
        ctx->scratchpad[sp_offset + i*2 + 1].value = (int64_t)snap[i].consecutive_failures;
    }

    ctx->reg[count_reg].type = REG_INT;
    ctx->reg[count_reg].i = n;
    return VM_OK;
}
