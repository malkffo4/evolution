// runtime/ops/cognitive.c
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>

#include "core/globals.h"
#include "opcode.h"
#include "math/hash.h"
#include "runtime/vm/vm.h"
#include "runtime/vm/vm_pool.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/logging/logging.h"
#include "runtime/time/time.h"
#include "knowledge/episode.h"
#include "knowledge/algorithm_loader.h"
#include "knowledge/evaluation.h"
#include "storage/db/db.h"
#include "storage/vector_store/vector_store.h"
#include "storage/string_pool/string_pool.h"
#include "memory/working.h"
#include "memory/critic_state.h"
#include "memory/subconscious.h"
#include "reasoning/planner.h"
#include "reasoning/algorithm_planner.h"

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
        pipeline_free(algo_pipeline);
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
    pipeline_free(algo_pipeline);
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
    uint32_t threshold_reg = ins->arg[1];
    uint32_t dst_reg = ins->arg[2];

    if (target_reg >= VM_MAX_REGISTERS || threshold_reg >= VM_MAX_REGISTERS || dst_reg >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    node_id_t target_id = (ctx->reg[target_reg].type == REG_NODE)
        ? ctx->reg[target_reg].node
        : (node_id_t)ctx->reg[target_reg].i;

    float threshold = (ctx->reg[threshold_reg].type == REG_FLOAT)
        ? (float)ctx->reg[threshold_reg].f
        : 0.7f;

    Vector128 target_vec;
    if (hyper_vector_load(ctx->memory.txn, db.graph.hyper.idx_vectors, target_id, &target_vec) != 0)
        return VM_NOT_FOUND;

    // Запрашиваем 8 кандидатов за O(log N) + легкий скан соседей по хэшу
    // Было MVP, стало ANN
    uint64_t results[8];
    int count = find_similar_nodes(ctx->memory.txn, target_vec.data, 8, results);

    if (count <= 0) return VM_NOT_FOUND;

    ko_id_t best_id = 0;
    // find_similar_nodes УЖЕ отсортировал результаты по убыванию косинусного сходства (Min-Heap).
    // Нам нужно просто взять первый элемент, который не равен самому target_id,
    // и убедиться, что он проходит threshold.
    for (int i = 0; i < count; i++) {
        if (results[i] != target_id) {
            Vector128 cand_vec;
            if (hyper_vector_load(ctx->memory.txn, db.graph.hyper.idx_vectors, results[i], &cand_vec) == 0) {
                float sim = vector_cosine_similarity(&target_vec, &cand_vec);
                if (sim >= threshold) {
                    best_id = results[i];
                    break;
                }
            }
        }
    }

    if (best_id == 0) return VM_NOT_FOUND;

    ctx->reg[dst_reg].type = REG_NODE;
    ctx->reg[dst_reg].node = best_id;
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
    if (!ctx->memory.wm || !ctx->memory.txn || !ctx->hyper_mem) return VM_ERROR;

    // Ядро больше ничего не "планирует". Оно просто тупо берет пайплайн CorePlanner из LMDB.
    uint64_t core_planner_id = djb2_hash("CorePlanner");
    Pipeline *planner_pipeline = NULL;

    if (algorithm_load(ctx->memory.txn, core_planner_id, &planner_pipeline) == 0 && planner_pipeline) {
        if (ctx->frame + 1 >= VM_MAX_CALL_DEPTH) {
            pipeline_free(planner_pipeline);
            return VM_STACK_OVERFLOW;
        }

        uint32_t prev_frame = ctx->frame;
        bool prev_halted = ctx->halted;

        ctx->frame++;
        VMFrame *f = &ctx->frames[ctx->frame];
        f->pipeline = planner_pipeline;
        f->code = planner_pipeline->code;
        f->ip = 0;
        ctx->halted = false;

        int rc = vm_execute(ctx, planner_pipeline);

        ctx->frame = prev_frame;
        ctx->halted = prev_halted;
        pipeline_free(planner_pipeline);

        return rc;
    }

    // Если CorePlanner не загружен в базу — мы просто молча ждем.
    return VM_NOT_FOUND;
}

int vm_op_load_context(VMContext *ctx, const Instruction *ins) {
    (void)ins;
    if (!ctx->hyper_mem || !ctx->memory.wm) return VM_ERROR;

    // Всегда загружаем контекст для всех активных узлов WM,
    // наличие цели необязательно — алгоритмы могут работать с любыми узлами.
    ctx->preloaded_edge_count = 0;

    for (uint32_t i = 0; i < ctx->memory.wm->count && ctx->preloaded_edge_count < MAX_PRELOADED_EDGES; i++) {
        node_id_t nid = ctx->memory.wm->nodes[i].node_id;

        NeuroAtom *fwd_atoms = NULL;
        size_t fwd_count = 0;

        if (hyper_find_by_participant(ctx->memory.txn, ctx->hyper_mem, nid, 0, &fwd_atoms, &fwd_count) == 0) {
            for (size_t j = 0; j < fwd_count && ctx->preloaded_edge_count < MAX_PRELOADED_EDGES; j++) {
                if (fwd_atoms[j].process_id == djb2_hash("EDGE_FWD")) {
                    node_id_t rel = HYPER_GET_ID(fwd_atoms[j].args[1].raw);

                    NeuroAtom *rev_atoms = NULL;
                    size_t rev_count = 0;

                    if (hyper_find_by_participant(ctx->memory.txn, ctx->hyper_mem, rel, 0, &rev_atoms, &rev_count) == 0) {
                        for (size_t k = 0; k < rev_count && ctx->preloaded_edge_count < MAX_PRELOADED_EDGES; k++) {
                            if (rev_atoms[k].process_id == djb2_hash("EDGE_REV") &&
                                HYPER_GET_ID(rev_atoms[k].args[0].raw) == rel) {

                                ctx->preloaded_edges[ctx->preloaded_edge_count++] = (CachedEdge){
                                    .source = nid,
                                    .relation = rel,
                                    .target = HYPER_GET_ID(rev_atoms[k].args[1].raw)
                                };
                            }
                        }
                        free(rev_atoms);
                    }
                }
            }
            free(fwd_atoms);
        }
    }
    return VM_OK;
}
