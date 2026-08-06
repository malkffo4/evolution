// runtime/ops/critic_ops.c
#include <stdint.h>
#include <time.h>

#include "runtime/logging/logging.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "memory/critic_state.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"
#include "knowledge/evaluation.h"

#define FLAW_THRESHOLD 3

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

// Пороговая проверка: если алгоритм провалился >= FLAW_THRESHOLD раз подряд,
// материализуем это как знание (HAS_FLAW) и понижаем доверие (CONFIDENCE_DELTA),
// а не просто держим состояние в оперативном quarantine_list.
int vm_op_critic_apply(VMContext *ctx, const Instruction *ins) {
    (void)ins;
    if (!ctx->hyper_mem || !ctx->memory.txn) return VM_ERROR;

    FailureSnapshot snap[MAX_QUARANTINE_NODES];
    uint32_t n = critic_dump_failures(snap, MAX_QUARANTINE_NODES);
    if (n == 0) return VM_OK;

    MDB_txn *txn = ctx->memory.txn;   // <-- the actual owning transaction of this VMContext
    node_id_t proc_flaw  = proc_make(djb2_hash("HAS_FLAW"), PROC_KIND_RELATION);
    node_id_t proc_delta = proc_make(djb2_hash("CONFIDENCE_DELTA"), PROC_KIND_RELATION);
    node_id_t garbage_concept = djb2_hash("GarbageCandidate"); // kept for symmetry w/ mark_flaw path
    (void)garbage_concept;

    for (uint32_t i = 0; i < n; i++) {
        if (snap[i].consecutive_failures < FLAW_THRESHOLD) continue;

        NeuroAtom flaw = {0};
        flaw.id          = hyper_memory_new_id(ctx->hyper_mem);   // was `now ^ ...`, collision-prone
        flaw.process_id  = proc_flaw;
        flaw.args[0].raw = HYPER_MAKE_REF(snap[i].algo_id);
        flaw.truth_mean       = 1.0f;
        flaw.truth_confidence = 1.0f;
        flaw.sti = 0.5f;
        flaw.lti = 0.30f;   // survives a few decay ticks so Learning can see it

        ko_id_t cause_flaw = ctx->current_episode_id;
        if (hyper_assert_with_cause(txn, ctx->hyper_mem, &flaw, cause_flaw) < 0) {
            LOG_ERROR("[CRITIC] failed to assert HAS_FLAW for algo=%lu", (unsigned long)snap[i].algo_id);
            continue;
        }

        NeuroAtom delta = {0};
        delta.id          = hyper_memory_new_id(ctx->hyper_mem);
        delta.process_id  = proc_delta;
        delta.args[0].raw = HYPER_MAKE_REF(snap[i].algo_id);
        union { float f; uint32_t i; } u; u.f = -0.2f;
        delta.args[1].raw = (ko_id_t)u.i | HYPER_TYPE_FLOAT;
        delta.truth_mean       = 1.0f;
        delta.truth_confidence = 0.8f;
        delta.sti = 0.5f;
        delta.lti = 0.1f;

        hyper_assert_with_cause(txn, ctx->hyper_mem, &delta, flaw.id);

        // This is the part that was pure decoration before: actually apply the
        // penalty to the Score rollup so Planner UCB1 sees it *this tick*,
        // not on the next accidental score_update() from vm_pool.
        score_update(txn, ctx->hyper_mem, COGNITIVE_DOMAIN_ALGORITHM,
                     snap[i].algo_id, 0.0f, flaw.id, 0);

        LOG_PLANNER("[CRITIC] algo=%lu marked HAS_FLAW after %d failures",
                    (unsigned long)snap[i].algo_id, snap[i].consecutive_failures);
    }
    return VM_OK;
}

/* OP_CREDIT_ASSIGN
 * arg[0] = domain (CognitiveDomain, immediate)
 * arg[1] = регистр с result_atom_id (REG_NODE/REG_INT)
 * arg[2] = регистр с outcome (REG_FLOAT)
 * arg[3] = max_depth (immediate, 0 = дефолт)
 * arg[4] = discount, упакован как float-биты (см. OP_MERGE_CTX для паттерна)
 * arg[5] = регистр-приёмник количества обновлённых Score (REG_INT)
 *
 * Распределяет credit по causal-цепочке результата. Не решает САМ,
 * когда его вызывать — это ответственность пайплайна (CriticMain),
 * что сохраняет принцип "когниция не хардкодится в kernel".
 */
int vm_op_credit_assign(VMContext *ctx, const Instruction *ins) {
    uint32_t domain      = ins->arg[0];
    uint32_t r_result    = ins->arg[1];
    uint32_t r_outcome   = ins->arg[2];
    uint32_t max_depth   = ins->arg[3];
    uint32_t r_out_count = ins->arg[5];

    if (r_result >= VM_MAX_REGISTERS || r_outcome >= VM_MAX_REGISTERS ||
        r_out_count >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if (!ctx->hyper_mem) return VM_ERROR;

    if (ctx->reg[r_result].type != REG_NODE && ctx->reg[r_result].type != REG_INT)
        return VM_INVALID_TYPE;
    if (ctx->reg[r_outcome].type != REG_FLOAT)
        return VM_INVALID_TYPE;

    node_id_t result_id = (ctx->reg[r_result].type == REG_NODE)
        ? ctx->reg[r_result].node : (node_id_t)ctx->reg[r_result].i;
    float outcome  = (float)ctx->reg[r_outcome].f;
    float discount = *(const float*)&ins->arg[4];

    int propagated = score_propagate_credit(ctx->memory.txn, ctx->hyper_mem, (CognitiveDomain)domain,
                                             result_id, outcome, max_depth, discount);
    if (propagated < 0) return VM_ERROR;

    ctx->reg[r_out_count].type = REG_INT;
    ctx->reg[r_out_count].i = propagated;
    return VM_OK;
}
