// runtime/ops/critic_ops.c
#include <stdint.h>
#include <time.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "memory/critic_state.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"
#include "runtime/logging/logging.h"

#define FLAW_THRESHOLD 3

// Пороговая проверка: если алгоритм провалился >= FLAW_THRESHOLD раз подряд,
// материализуем это как знание (HAS_FLAW) и понижаем доверие (CONFIDENCE_DELTA),
// а не просто держим состояние в оперативном quarantine_list.
int vm_op_critic_apply(VMContext *ctx, const Instruction *ins) {
    (void)ins;
    if (!ctx->hyper_mem) return VM_ERROR;

    FailureSnapshot snap[MAX_QUARANTINE_NODES];
    uint32_t n = critic_dump_failures(snap, MAX_QUARANTINE_NODES);

    node_id_t proc_flaw  = djb2_hash("HAS_FLAW");
    node_id_t proc_delta = djb2_hash("CONFIDENCE_DELTA");
    uint64_t now = (uint64_t)time(NULL);

    for (uint32_t i = 0; i < n; i++) {
        if (snap[i].consecutive_failures < FLAW_THRESHOLD) continue;

        HyperAtom flaw = {0};
        flaw.id = now ^ (snap[i].algo_id << 1) ^ 0xF1A3;
        flaw.process_id = proc_flaw;
        flaw.args[0].raw = HYPER_MAKE_REF(snap[i].algo_id);
        flaw.context_id = 0;
        flaw.time_tick = ctx->cycles;
        flaw.cause_id = ctx->current_episode_id;
        hyper_assert_unique(ctx->hyper_mem, &flaw);

        HyperAtom delta = {0};
        delta.id = now ^ (snap[i].algo_id << 2) ^ 0xD3A7A;
        delta.process_id = proc_delta;
        delta.args[0].raw = HYPER_MAKE_REF(snap[i].algo_id);
        union { float f; uint32_t i; } u; u.f = -0.2f;
        delta.args[1].raw = (ko_id_t)u.i | HYPER_TYPE_FLOAT;
        delta.context_id = 0;
        delta.time_tick = ctx->cycles;
        delta.cause_id = flaw.id;
        hyper_assert_unique(ctx->hyper_mem, &delta);

        LOG_PLANNER("[CRITIC] algo=%lu marked HAS_FLAW after %d failures",
                    (unsigned long)snap[i].algo_id, snap[i].consecutive_failures);
    }
    return VM_OK;
}
