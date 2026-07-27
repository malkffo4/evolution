// runtime/ops/pattern_ops.c
#include <string.h>
#include <stdlib.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "storage/hyper_atom/hyper_pattern.h"
#include "storage/db/db.h"
#include "runtime/logging/logging.h"

#define MATCH_BUDGET 4096  // лимит просмотренных атомов за один вызов — защита от runaway backtracking

typedef struct {
    ko_id_t bound[MAX_PATTERN_VARS];
    bool    is_bound[MAX_PATTERN_VARS];
} Bindings;

static bool unify_slot(const PatternCondition *cond, int slot, ko_id_t real_val, Bindings *b) {
    switch (cond->kind[slot]) {
        case PARG_ANY:
            return true;
        case PARG_CONST:
            return cond->value[slot] == real_val;
        case PARG_VAR: {
            uint8_t v = cond->var_index[slot];
            if (v >= MAX_PATTERN_VARS) return false;
            if (!b->is_bound[v]) { b->is_bound[v] = true; b->bound[v] = real_val; return true; }
            return b->bound[v] == real_val;
        }
    }
    return false;
}

static void unbind_slot(const PatternCondition *cond, int slot, Bindings *b, bool was_bound_before) {
    if (cond->kind[slot] == PARG_VAR) {
        uint8_t v = cond->var_index[slot];
        if (v < MAX_PATTERN_VARS && !was_bound_before) { b->is_bound[v] = false; b->bound[v] = 0; }
    }
}

typedef struct {
    VMContext *ctx;
    uint32_t   sp_base;
    uint32_t   var_count;
    uint32_t   max_results;
    uint32_t   found;
} MatchSink;

static void emit_match(MatchSink *sink, const Bindings *b) {
    if (sink->found >= sink->max_results) return;
    uint32_t base = sink->sp_base + sink->found * sink->var_count;
    for (uint32_t v = 0; v < sink->var_count; v++) {
        if (base + v >= MAX_SCRATCHPAD) break;
        sink->ctx->scratchpad[base + v].key_hash = v;
        sink->ctx->scratchpad[base + v].value = (int64_t)(b->is_bound[v] ? b->bound[v] : 0);
    }
    sink->found++;
}

static int match_recursive(HyperMemory *hmem, const HyperPattern *pat, uint32_t cond_idx,
                            ko_id_t context_filter, Bindings *b, MatchSink *sink, uint32_t *budget) {
    if (sink->found >= sink->max_results) return VM_OK;

    if (cond_idx >= pat->condition_count) {
        emit_match(sink, b);
        return VM_OK;
    }

    const PatternCondition *cond = &pat->conditions[cond_idx];

    // Если один из слотов уже известен как REF (константа или уже связанная переменная) —
    // используем его как participant-фильтр, чтобы не сканировать весь process_id.
    ko_id_t known_participant = 0;
    for (int s = 0; s < 3; s++) {
        if (cond->kind[s] == PARG_CONST && HYPER_GET_TYPE(cond->value[s]) == HYPER_TYPE_REF) {
            known_participant = HYPER_GET_ID(cond->value[s]); break;
        }
        if (cond->kind[s] == PARG_VAR && b->is_bound[cond->var_index[s]]) {
            ko_id_t bv = b->bound[cond->var_index[s]];
            if (HYPER_GET_TYPE(bv) == HYPER_TYPE_REF) { known_participant = HYPER_GET_ID(bv); break; }
        }
    }

    HyperAtom *candidates = NULL;
    size_t count = 0;
    if (hyper_find_by_process(hmem, cond->process_id, known_participant, context_filter,
                               &candidates, &count) != 0)
        return VM_OK; // тупиковая ветка — не ошибка VM

    int rc = VM_OK;
    for (size_t i = 0; i < count; i++) {
        if (*budget == 0) { rc = VM_TIMEOUT; break; }
        (*budget)--;

        HyperAtom *a = &candidates[i];
        if (context_filter != 0 && a->context_id != context_filter) continue;

        bool prebound[3];
        bool ok = true;
        for (int s = 0; s < 3 && ok; s++) {
            prebound[s] = (cond->kind[s] == PARG_VAR) && b->is_bound[cond->var_index[s]];
            ok = unify_slot(cond, s, a->args[s].raw, b);
        }

        if (ok) {
            rc = match_recursive(hmem, pat, cond_idx + 1, context_filter, b, sink, budget);
            if (rc != VM_OK) break;
        }

        for (int s = 0; s < 3; s++) unbind_slot(cond, s, b, prebound[s]);
        if (sink->found >= sink->max_results) break;
    }

    free(candidates);
    return rc;
}

// arg[0]=reg(pattern_id) arg[1]=reg(context_id, 0=без фильтра) arg[2]=sp_base
// arg[3]=reg<-match_count arg[4]=reg<-var_count arg[5]=max_results (immediate)
int vm_op_match_pattern(VMContext *ctx, const Instruction *ins) {
    uint32_t r_pattern = ins->arg[0], r_ctx = ins->arg[1], sp_base = ins->arg[2];
    uint32_t r_count = ins->arg[3], r_varcount = ins->arg[4], max_results = ins->arg[5];

    if (r_pattern >= VM_MAX_REGISTERS || r_ctx >= VM_MAX_REGISTERS ||
        r_count >= VM_MAX_REGISTERS || r_varcount >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if (!ctx->hyper_mem) return VM_ERROR;
    if (sp_base >= MAX_SCRATCHPAD) return VM_INVALID_REGISTER;

    ko_id_t pattern_id = (ko_id_t)ctx->reg[r_pattern].i;
    ko_id_t context_filter = (ko_id_t)ctx->reg[r_ctx].i;
    if (max_results == 0) max_results = 64;

    HyperPattern pattern;
    if (hyper_pattern_load(ctx->memory.txn, db.graph.hyper.patterns, pattern_id, &pattern) != MDB_SUCCESS) {
        LOG_WARN("OP_MATCH_PATTERN: pattern %lu not found", (unsigned long)pattern_id);
        return VM_NOT_FOUND;
    }

    uint32_t slots_left = MAX_SCRATCHPAD - sp_base;
    uint32_t cap = pattern.var_count ? slots_left / pattern.var_count : slots_left;
    if (max_results > cap) max_results = cap;

    Bindings b = {0};
    MatchSink sink = { .ctx = ctx, .sp_base = sp_base, .var_count = pattern.var_count,
                        .max_results = max_results, .found = 0 };
    uint32_t budget = MATCH_BUDGET;

    int rc = match_recursive(ctx->hyper_mem, &pattern, 0, context_filter, &b, &sink, &budget);

    ctx->reg[r_count].type = REG_INT;    ctx->reg[r_count].i = (int64_t)sink.found;
    ctx->reg[r_varcount].type = REG_INT; ctx->reg[r_varcount].i = (int64_t)pattern.var_count;
    return rc;
}
