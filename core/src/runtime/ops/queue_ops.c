// runtime/ops/queue_ops.c
//
// Универсальный примитив "отложенная очередь + инкрементальная оценка".
// НЕ специфичен для пентеста: домен заводит свою очередь через
// djb2_hash("<domain>::<queue>") и материализует над ней собственный
// Pipeline-классификатор — то же разделение, что уже есть между
// InductiveExtractor и его доменом.
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "knowledge/event_queue.h"
#include "knowledge/evaluation.h"

// OP_QUEUE_PUSH: arg[0]=queue_id_reg, arg[1]=atom_id_reg
int vm_op_queue_push(VMContext *ctx, const Instruction *ins) {
    uint32_t r_queue = ins->arg[0], r_atom = ins->arg[1];
    if (r_queue >= VM_MAX_REGISTERS || r_atom >= VM_MAX_REGISTERS) return VM_INVALID_REGISTER;
    if (!ctx->hyper_mem) return VM_ERROR;

    ko_id_t queue_id = (ctx->reg[r_queue].type == REG_NODE) ? ctx->reg[r_queue].node : (ko_id_t)ctx->reg[r_queue].i;
    ko_id_t atom_id  = (ctx->reg[r_atom].type  == REG_NODE) ? ctx->reg[r_atom].node  : (ko_id_t)ctx->reg[r_atom].i;
    if (queue_id == 0 || atom_id == 0) return VM_INVALID_TYPE;

    return (event_queue_push(ctx->memory.txn, ctx->hyper_mem, queue_id, atom_id) == 0) ? VM_OK : VM_ERROR;
}

// OP_QUEUE_POP: arg[0]=queue_id_reg, arg[1]=sp_base, arg[2]=max_count(imm), arg[3]=dst_count_reg
int vm_op_queue_pop(VMContext *ctx, const Instruction *ins) {
    uint32_t r_queue = ins->arg[0], sp_base = ins->arg[1];
    uint32_t max_count = ins->arg[2], r_count = ins->arg[3];

    if (r_queue >= VM_MAX_REGISTERS || r_count >= VM_MAX_REGISTERS) return VM_INVALID_REGISTER;
    if (!ctx->hyper_mem) return VM_ERROR;
    if (max_count == 0) max_count = 32;
    if (max_count > EVENT_QUEUE_MAX_POP) max_count = EVENT_QUEUE_MAX_POP;
    if (sp_base >= MAX_SCRATCHPAD) return VM_INVALID_REGISTER;
    if (sp_base + max_count > MAX_SCRATCHPAD) max_count = MAX_SCRATCHPAD - sp_base;

    ko_id_t queue_id = (ctx->reg[r_queue].type == REG_NODE) ? ctx->reg[r_queue].node : (ko_id_t)ctx->reg[r_queue].i;

    ko_id_t popped[EVENT_QUEUE_MAX_POP];
    int n = event_queue_pop_batch(ctx->memory.txn, ctx->hyper_mem, queue_id, popped, max_count);
    if (n < 0) return VM_ERROR;

    for (int i = 0; i < n; i++) {
        ctx->scratchpad[sp_base + i].key_hash = 0;
        ctx->scratchpad[sp_base + i].value = (int64_t)popped[i];
    }
    ctx->reg[r_count].type = REG_INT;
    ctx->reg[r_count].i = n;
    return VM_OK;
}

// OP_SCORE_UPDATE: arg[0]=domain(imm), arg[1]=subject_reg, arg[2]=outcome_reg(FLOAT),
// arg[3]=cause_reg(0 допустим), arg[4]=context_reg(0 допустим)
//
// Единственный способ ЛЮБОМУ материализованному в LMDB Pipeline (не только
// C-коду vm_pool.c/critic_ops.c) инкрементально копить доверие к субъекту,
// не раздувая граф новым Hypothesis-атомом на каждое наблюдение — само
// наблюдение уже фиксируется evaluation_record() внутри score_update().
int vm_op_score_update(VMContext *ctx, const Instruction *ins) {
    uint32_t domain = ins->arg[0];
    uint32_t r_subject = ins->arg[1], r_outcome = ins->arg[2];
    uint32_t r_cause = ins->arg[3], r_context = ins->arg[4];

    if (r_subject >= VM_MAX_REGISTERS || r_outcome >= VM_MAX_REGISTERS ||
        (r_cause && r_cause >= VM_MAX_REGISTERS) || (r_context && r_context >= VM_MAX_REGISTERS))
        return VM_INVALID_REGISTER;
    if (!ctx->hyper_mem) return VM_ERROR;
    if (ctx->reg[r_outcome].type != REG_FLOAT) return VM_INVALID_TYPE;

    node_id_t subject = (ctx->reg[r_subject].type == REG_NODE) ? ctx->reg[r_subject].node : (node_id_t)ctx->reg[r_subject].i;
    float outcome = (float)ctx->reg[r_outcome].f;

    node_id_t cause_id = 0;
    if (r_cause && (ctx->reg[r_cause].type == REG_NODE || ctx->reg[r_cause].type == REG_INT))
        cause_id = (ctx->reg[r_cause].type == REG_NODE) ? ctx->reg[r_cause].node : (node_id_t)ctx->reg[r_cause].i;

    node_id_t context_id = 0;
    if (r_context && (ctx->reg[r_context].type == REG_NODE || ctx->reg[r_context].type == REG_INT))
        context_id = (ctx->reg[r_context].type == REG_NODE) ? ctx->reg[r_context].node : (node_id_t)ctx->reg[r_context].i;

    int rc = score_update(ctx->memory.txn, ctx->hyper_mem, (CognitiveDomain)domain,
                           subject, outcome, cause_id, context_id);
    return (rc == 0) ? VM_OK : VM_ERROR;
}
