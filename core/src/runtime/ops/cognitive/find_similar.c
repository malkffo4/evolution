// src/runtime/ops/cognitive/find_similar.c
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "storage/vector_store/vector_store.h"

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
