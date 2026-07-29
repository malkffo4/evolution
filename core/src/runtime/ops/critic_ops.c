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
    if (!ctx->hyper_mem) return VM_ERROR;

    FailureSnapshot snap[MAX_QUARANTINE_NODES];
    uint32_t n = critic_dump_failures(snap, MAX_QUARANTINE_NODES);
    if (n == 0) return VM_OK;

    node_id_t proc_flaw  = djb2_hash("HAS_FLAW");
    node_id_t proc_delta = djb2_hash("CONFIDENCE_DELTA");
    uint64_t now = (uint64_t)time(NULL);   // для генерации id атомов

    for (uint32_t i = 0; i < n; i++) {
        if (snap[i].consecutive_failures < FLAW_THRESHOLD) continue;

        // --- HAS_FLAW ---
        NeuroAtom flaw = {0};
        flaw.id          = now ^ (snap[i].algo_id << 1) ^ 0xF1A3;
        flaw.process_id  = proc_flaw;
        flaw.args[0].raw = HYPER_MAKE_REF(snap[i].algo_id);
        // args[1] остаётся нулём
        flaw.context_or_time_link = 0;          // можно задать контекст, если нужно
        // Эпистемический вектор – факт считаем абсолютно истинным
        flaw.truth_mean       = 1.0f;
        flaw.truth_confidence = 1.0f;
        flaw.sti              = 0.5f;
        flaw.lti              = 0.1f;

        // Причина: текущий эпизод (или 0, если контекст не задан)
        ko_id_t cause_flaw = ctx->current_episode_id;
        hyper_assert_with_cause(ctx->hyper_mem, &flaw, cause_flaw);

        // --- CONFIDENCE_DELTA ---
        NeuroAtom delta = {0};
        delta.id          = now ^ (snap[i].algo_id << 2) ^ 0xD3A7A;
        delta.process_id  = proc_delta;
        delta.args[0].raw = HYPER_MAKE_REF(snap[i].algo_id);
        // Упаковываем float -0.2 в args[1]
        union { float f; uint32_t i; } u;
        u.f = -0.2f;
        delta.args[1].raw = (ko_id_t)u.i | HYPER_TYPE_FLOAT;
        delta.context_or_time_link = 0;
        delta.truth_mean       = 1.0f;
        delta.truth_confidence = 0.8f;
        delta.sti              = 0.5f;
        delta.lti              = 0.1f;

        // Причина: атом HAS_FLAW, который мы только что создали
        ko_id_t cause_delta = flaw.id;
        hyper_assert_with_cause(ctx->hyper_mem, &delta, cause_delta);

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

    int propagated = score_propagate_credit(ctx->hyper_mem, (CognitiveDomain)domain,
                                             result_id, outcome, max_depth, discount);
    if (propagated < 0) return VM_ERROR;

    ctx->reg[r_out_count].type = REG_INT;
    ctx->reg[r_out_count].i = propagated;
    return VM_OK;
}
