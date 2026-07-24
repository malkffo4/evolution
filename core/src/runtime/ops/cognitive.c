// runtime/ops/cognitive.c
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <math.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "knowledge/algorithm_loader.h"
#include "storage/vector_store/vector_store.h"
#include "memory/working.h"
#include "reasoning/planner.h"

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

int vm_op_concat_paths(VMContext *ctx, const Instruction *ins) {
    uint32_t sp_dest = ins->arg[0];
    uint32_t r1 = ins->arg[1];
    uint32_t r2 = ins->arg[2];
    uint32_t r3 = ins->arg[3];
    uint32_t r4 = ins->arg[4];

    if (r1 >= VM_MAX_REGISTERS || r2 >= VM_MAX_REGISTERS ||
        r3 >= VM_MAX_REGISTERS || r4 >= VM_MAX_REGISTERS ||
        sp_dest >= MAX_SCRATCHPAD)
        return VM_INVALID_REGISTER;

    node_id_t n1 = (node_id_t)ctx->reg[r1].i;
    node_id_t n2 = (node_id_t)ctx->reg[r2].i;
    node_id_t n3 = (node_id_t)ctx->reg[r3].i;
    node_id_t n4 = (node_id_t)ctx->reg[r4].i;

    // Освобождаем предыдущую строку, если была и это действительно указатель
    if (ctx->scratchpad[sp_dest].value && (uintptr_t)ctx->scratchpad[sp_dest].value > 0x1000) {
        free((void*)(uintptr_t)ctx->scratchpad[sp_dest].value);
    }

    char *path_str = malloc(128);
    if (!path_str) return VM_OUT_OF_MEMORY;
    snprintf(path_str, 128, "Path(%lu -> %lu ~> %lu -> %lu)",
             (unsigned long)n1, (unsigned long)n2,
             (unsigned long)n3, (unsigned long)n4);

    ctx->scratchpad[sp_dest].value = (int64_t)(uintptr_t)path_str;
    ctx->scratchpad[sp_dest].key_hash = 0;
    return VM_OK;
}

int vm_op_exec_algorithm(VMContext *ctx, const Instruction *ins) {
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

    // Защита от переполнения стека
    if (ctx->frame + 1 >= VM_MAX_CALL_DEPTH) {
        free(algo_pipeline->code);
        free(algo_pipeline);
        return VM_STACK_OVERFLOW;
    }

    // Сохраняем состояние
    uint32_t prev_frame = ctx->frame;
    bool prev_halted = ctx->halted;

    // Переключаемся на новый фрейм
    ctx->frame++;
    VMFrame *f = &ctx->frames[ctx->frame];
    f->pipeline = algo_pipeline;
    f->code     = algo_pipeline->code;
    f->ip       = 0;

    ctx->halted = false;   // сбрасываем halted для подпрограммы
    rc = vm_execute(ctx, algo_pipeline);

    // Восстанавливаем состояние
    ctx->frame = prev_frame;
    ctx->halted = prev_halted;

    // Освобождаем загруженный pipeline
    free(algo_pipeline->code);
    free(algo_pipeline);
    return rc;
}

int vm_op_exec_algorithm_by_goal(VMContext *ctx, const Instruction *ins) {
    // ... загрузка по goal_id (в будущем) ...
    // Пока что такая же логика, что и exec_algorithm, для совместимости.
    // Но в будущем будет использоваться planner для выбора алгоритма по цели.
    return vm_op_exec_algorithm(ctx, ins);
}

int vm_op_find_similar(VMContext *ctx, const Instruction *ins) {
    uint32_t target_reg = ins->arg[0];
    uint32_t sp_dest    = ins->arg[3];

    if (target_reg >= VM_MAX_REGISTERS || sp_dest >= MAX_SCRATCHPAD)
        return VM_INVALID_REGISTER;

    node_id_t target = (ctx->reg[target_reg].type == REG_NODE)
        ? ctx->reg[target_reg].node
        : (node_id_t)ctx->reg[target_reg].i;

    // Ищем эмбеддинг таргета по всему scratchpad
    float *target_emb = NULL;
    for (uint32_t i = 0; i < MAX_SCRATCHPAD; i++) {
        if (ctx->scratchpad[i].key_hash == target && ctx->scratchpad[i].value) {
            target_emb = (float *)(uintptr_t)ctx->scratchpad[i].value;
            break;
        }
    }
    if (!target_emb) return VM_NOT_FOUND;

    float best_sim = -1.0f;
    node_id_t best_node = 0;

    // Сканируем весь scratchpad в поисках других эмбеддингов
    for (uint32_t i = 0; i < MAX_SCRATCHPAD; i++) {
        if (i == sp_dest) continue;
        float *cand_emb = (float *)(uintptr_t)ctx->scratchpad[i].value;
        node_id_t cand_node = ctx->scratchpad[i].key_hash;
        if (!cand_emb || cand_node == target || cand_node == 0) continue;

        float dot = 0.0f, norm1 = 0.0f, norm2 = 0.0f;
        for (int d = 0; d < EMBEDDING_DIM; d++) {
            dot   += target_emb[d] * cand_emb[d];
            norm1 += target_emb[d] * target_emb[d];
            norm2 += cand_emb[d] * cand_emb[d];
        }
        float sim = (norm1 > 0.0f && norm2 > 0.0f)
            ? dot / (sqrtf(norm1) * sqrtf(norm2))
            : 0.0f;

        if (sim > best_sim) {
            best_sim = sim;
            best_node = cand_node;
        }
    }

    ctx->scratchpad[sp_dest].key_hash = best_node;   // информативно
    ctx->scratchpad[sp_dest].value = (int64_t)best_node;  // <-- главное: читается через read_sp
    return VM_OK;
}

int vm_op_get_neighbors(VMContext *ctx, const Instruction *ins) {
    uint32_t src_reg = ins->arg[0];
    uint32_t rel_reg = ins->arg[1];
    uint32_t sp_start = ins->arg[2];
    uint32_t count_reg = ins->arg[3];

    if (src_reg >= VM_MAX_REGISTERS || rel_reg >= VM_MAX_REGISTERS ||
        count_reg >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    node_id_t src = (ctx->reg[src_reg].type == REG_NODE) ? ctx->reg[src_reg].node : (node_id_t)ctx->reg[src_reg].i;
    node_id_t rel = (ctx->reg[rel_reg].type == REG_NODE) ? ctx->reg[rel_reg].node : (node_id_t)ctx->reg[rel_reg].i;

    uint32_t count = 0;
    for (uint32_t i = 0; i < ctx->preloaded_edge_count; i++) {
        if (ctx->preloaded_edges[i].source == src &&
            ctx->preloaded_edges[i].relation == rel) {
            if (sp_start + count >= MAX_SCRATCHPAD) break;
            ctx->scratchpad[sp_start + count].key_hash = 0;
            ctx->scratchpad[sp_start + count].value = (int64_t)ctx->preloaded_edges[i].target;
            count++;
        }
    }

    ctx->reg[count_reg].type = REG_INT;
    ctx->reg[count_reg].i = count;
    return VM_OK;
}

int vm_op_spread_activation(VMContext *ctx, const Instruction *ins) {
    (void)ins;
    if (ctx->memory.wm && ctx->memory.txn)
        engine_spread_activation(ctx->memory.wm, ctx->memory.txn);
    return VM_OK;
}

int vm_op_evaluate_goals(VMContext *ctx, const Instruction *ins) {
    (void)ins;
    if (ctx->memory.wm && ctx->memory.txn)
        planner_evaluate_goals(ctx->memory.wm, ctx->memory.txn);
    return VM_OK;
}

int vm_op_read_sp(VMContext *ctx, const Instruction *ins) {
    uint32_t dst_reg = ins->arg[0];
    uint32_t sp_idx  = ins->arg[1];

    if (dst_reg >= VM_MAX_REGISTERS || sp_idx >= MAX_SCRATCHPAD)
        return VM_INVALID_REGISTER;

    ctx->reg[dst_reg].type = REG_INT;          // scratchpad хранит int64_t
    ctx->reg[dst_reg].i = ctx->scratchpad[sp_idx].value;

    return VM_OK;
}
