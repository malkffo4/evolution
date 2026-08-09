// runtime/ops/composition_ops.c
//
// Zero-Shot Algorithm Composition (алгоритмы, синтезирующие алгоритмы).
//
// Держит принцип Code=Data: РЕШЕНИЕ "когда и с чем компоновать" остаётся
// целиком в байткоде (ZeroShotComposer.pipeline, bootstrap.py). Здесь —
// только два ограниченных, детерминированных нативных примитива:
//
//   OP_FIND_PRODUCER_CHAIN — находит (A, X, B) такие что
//     PRODUCES(B, goal) и REQUIRES(B, X) и PRODUCES(A, X).
//     Поиск идёт ИСКЛЮЧИТЕЛЬНО через локальный fan-out конкретных узлов
//     (hyper_find_by_participant -> idx_args), а не через полный скан
//     отношения PRODUCES/REQUIRES по всей базе — то же ограничение по
//     масштабу, что уже используется в evaluation.c::find_score_atom()
//     и planner_ops.c::wm_node_is_typed_goal().
//
//   OP_SYNTHESIZE_SEQUENCE — строит НОВЫЙ исполняемый Pipeline
//     (LOAD_CONST A; EXEC_ALGORITHM A; LOAD_CONST B; EXEC_ALGORITHM B; HALT)
//     и сохраняет его через уже существующий algorithm_save() под свежим
//     id. Это чисто механическая сборка последовательной композиции двух
//     уже готовых Pipeline — реального планирования здесь нет.
//
// Валидация композиции НЕ дублирует существующий цикл исполнения: после
// того как ZeroShotComposer свяжет новый алгоритм с целью через
// HAS_ALGORITHM (с низкой начальной уверенностью, см. vm_op_derive),
// обычный CorePlanner/OP_SELECT_ALGORITHM/OP_DISPATCH_ASYNC на следующем
// тике исполнит его через штатный vm_pool — Episode/Score/Critic
// становятся честной "боевой" проверкой гипотезы без отдельного
// sandbox-исполнения внутри самого композитора.
//
// Потокобезопасность: НИКАКОГО static-кэша хэшей процессов — оба опкода
// могут вызываться параллельно из нескольких vm_pool worker-потоков
// одновременно (в отличие от planner_ops.c, который эксклюзивно
// вызывается из единственного потока dmn_loop).
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <lmdb.h>

#include "math/hash.h"
#include "runtime/logging/logging.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/ops/opcode.h"
#include "runtime/compiler/pipeline.h"
#include "knowledge/algorithm_saver.h"
#include "knowledge/algorithm_loader.h"
#include "storage/hyper_atom/hyper_atom.h"

// arg[0]=r_goal(src), arg[1]=r_algo_a(dst,0=не нужен), arg[2]=r_algo_b(dst),
// arg[3]=r_resource_x(dst), arg[4]=r_found(dst,0/1),
// arg[5]=r_cause(dst) — id атома PRODUCES(B,goal), провенанс для
// последующего OP_DERIVE (Principle 9: "Любое знание имеет происхождение").
int vm_op_find_producer_chain(VMContext *ctx, const Instruction *ins) {
    uint32_t r_goal   = ins->arg[0];
    uint32_t r_algo_a = ins->arg[1];
    uint32_t r_algo_b = ins->arg[2];
    uint32_t r_res_x  = ins->arg[3];
    uint32_t r_found  = ins->arg[4];
    uint32_t r_cause  = ins->arg[5];

    if (r_goal >= VM_MAX_REGISTERS || r_algo_a >= VM_MAX_REGISTERS ||
        r_algo_b >= VM_MAX_REGISTERS || r_res_x >= VM_MAX_REGISTERS ||
        r_found >= VM_MAX_REGISTERS || r_cause >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if (!ctx->hyper_mem || !ctx->memory.txn) return VM_ERROR;
    if (ctx->reg[r_goal].type != REG_NODE && ctx->reg[r_goal].type != REG_INT)
        return VM_INVALID_TYPE;

    node_id_t goal_id = (ctx->reg[r_goal].type == REG_NODE)
        ? ctx->reg[r_goal].node : (node_id_t)ctx->reg[r_goal].i;

    // Пересчитываем на каждый вызов — см. комментарий о потокобезопасности выше.
    node_id_t proc_produces = proc_make(djb2_hash("PRODUCES"),      PROC_KIND_RELATION);
    node_id_t proc_requires = proc_make(djb2_hash("REQUIRES"),      PROC_KIND_RELATION);
    node_id_t proc_has_algo = proc_make(djb2_hash("HAS_ALGORITHM"), PROC_KIND_RELATION);

    ctx->reg[r_found].type = REG_INT;
    ctx->reg[r_found].i = 0;

    // Шаг 1: локальный fan-out цели — НЕ скан всего отношения PRODUCES.
    NeuroAtom *goal_atoms = NULL;
    size_t goal_atom_count = 0;
    if (hyper_find_by_participant(ctx->memory.txn, ctx->hyper_mem, goal_id, 0,
                                   &goal_atoms, &goal_atom_count) != 0) {
        return VM_OK; // нет данных — не ошибка, просто нечего компоновать
    }

    for (size_t i = 0; i < goal_atom_count && !ctx->reg[r_found].i; i++) {
        NeuroAtom *pb = &goal_atoms[i];
        if (pb->process_id != proc_produces) continue;
        if (HYPER_GET_TYPE(pb->args[1].raw) != HYPER_TYPE_REF ||
            HYPER_GET_ID(pb->args[1].raw) != goal_id) continue;
        if (HYPER_GET_TYPE(pb->args[0].raw) != HYPER_TYPE_REF) continue;

        node_id_t cand_b = HYPER_GET_ID(pb->args[0].raw);
        if (cand_b == goal_id) continue;

        // Шаг 2: локальный fan-out кандидата B.
        NeuroAtom *b_atoms = NULL;
        size_t b_count = 0;
        if (hyper_find_by_participant(ctx->memory.txn, ctx->hyper_mem, cand_b, 0,
                                       &b_atoms, &b_count) != 0)
            continue;

        // Если B УЖЕ напрямую привязан к цели — это не наша задача,
        // обычный select_algorithm с ним справится сам.
        bool already_linked = false;
        for (size_t j = 0; j < b_count && !already_linked; j++) {
            if (b_atoms[j].process_id == proc_has_algo &&
                HYPER_GET_ID(b_atoms[j].args[0].raw) == cand_b &&
                HYPER_GET_ID(b_atoms[j].args[1].raw) == goal_id) {
                already_linked = true;
            }
        }
        if (already_linked) { free(b_atoms); continue; }

        for (size_t j = 0; j < b_count && !ctx->reg[r_found].i; j++) {
            if (b_atoms[j].process_id != proc_requires) continue;
            if (HYPER_GET_TYPE(b_atoms[j].args[0].raw) != HYPER_TYPE_REF ||
                HYPER_GET_ID(b_atoms[j].args[0].raw) != cand_b) continue;
            if (HYPER_GET_TYPE(b_atoms[j].args[1].raw) != HYPER_TYPE_REF) continue;

            node_id_t resource_x = HYPER_GET_ID(b_atoms[j].args[1].raw);

            // Шаг 3: локальный fan-out ресурса X.
            NeuroAtom *x_atoms = NULL;
            size_t x_count = 0;
            if (hyper_find_by_participant(ctx->memory.txn, ctx->hyper_mem, resource_x, 0,
                                           &x_atoms, &x_count) == 0) {
                for (size_t k = 0; k < x_count; k++) {
                    if (x_atoms[k].process_id != proc_produces) continue;
                    if (HYPER_GET_TYPE(x_atoms[k].args[1].raw) != HYPER_TYPE_REF ||
                        HYPER_GET_ID(x_atoms[k].args[1].raw) != resource_x) continue;
                    if (HYPER_GET_TYPE(x_atoms[k].args[0].raw) != HYPER_TYPE_REF) continue;

                    node_id_t cand_a = HYPER_GET_ID(x_atoms[k].args[0].raw);
                    if (cand_a == cand_b || cand_a == resource_x || cand_a == goal_id) continue;

                    ctx->reg[r_algo_a].type = REG_NODE; ctx->reg[r_algo_a].node = cand_a;
                    ctx->reg[r_algo_b].type = REG_NODE; ctx->reg[r_algo_b].node = cand_b;
                    ctx->reg[r_res_x].type  = REG_NODE; ctx->reg[r_res_x].node  = resource_x;
                    ctx->reg[r_cause].type  = REG_INT;  ctx->reg[r_cause].i     = (int64_t)pb->id;
                    ctx->reg[r_found].i = 1;

                    LOG_REASONER("[ZERO_SHOT] chain found goal=%lu: A=%lu produces X=%lu, "
                                 "B=%lu requires X and produces goal (cause_atom=%lu)",
                                 (unsigned long)goal_id, (unsigned long)cand_a,
                                 (unsigned long)resource_x, (unsigned long)cand_b,
                                 (unsigned long)pb->id);
                    break;
                }
            }
            if (x_atoms) free(x_atoms);
        }
        free(b_atoms);
    }
    free(goal_atoms);

    if (!ctx->reg[r_found].i) {
        LOG_REASONER("[ZERO_SHOT] no producer chain found for goal=%lu", (unsigned long)goal_id);
    }

    return VM_OK;
}

// arg[0]=r_algo_a(src, REG_NODE/INT, 0=пропустить первый шаг),
// arg[1]=r_algo_b(src, REG_NODE/INT, обязателен),
// arg[2]=r_dst(dst) — id нового составного алгоритма.
//
// Строит Pipeline: [LOAD_CONST A; EXEC_ALGORITHM A;] LOAD_CONST B; EXEC_ALGORITHM B; HALT
// и сохраняет его через algorithm_save() под свежим id (hyper_memory_new_id).
// A и B исполняются ПОСЛЕДОВАТЕЛЬНО в ОДНОМ VMContext (vm_op_exec_algorithm
// не сохраняет/не восстанавливает регистровый файл вокруг вложенного
// vm_execute — только ctx->frame/ctx->halted) — поэтому R0 естественно
// работает как канал передачи результата A -> вход B, без специального
// протокола данных.
/*
 * Zero-shot composition:
 *   A -> B
 * I/O contract is taken directly from Pipeline:
 *   A.out_regs[0] -> B.in_regs[0]
 * If a pipeline has no explicit I/O metadata, R0 is the legacy default.
 * No algorithm-specific knowledge is stored here.
 */
int vm_op_synthesize_sequence(VMContext *ctx, const Instruction *ins) {
    uint32_t r_algo_a = ins->arg[0];
    uint32_t r_algo_b = ins->arg[1];
    uint32_t r_dst    = ins->arg[2];

    if (r_algo_a >= VM_MAX_REGISTERS || r_algo_b >= VM_MAX_REGISTERS || r_dst >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    if (!ctx->hyper_mem || !ctx->memory.txn) return VM_ERROR;

    if (ctx->reg[r_algo_b].type != REG_NODE && ctx->reg[r_algo_b].type != REG_INT)
        return VM_INVALID_TYPE;

    node_id_t algo_b = (ctx->reg[r_algo_b].type == REG_NODE)
        ? ctx->reg[r_algo_b].node : (node_id_t)ctx->reg[r_algo_b].i;

    if (algo_b == 0) return VM_INVALID_TYPE;

    node_id_t algo_a = 0;

    if (ctx->reg[r_algo_a].type == REG_NODE) algo_a = ctx->reg[r_algo_a].node;
    else if (ctx->reg[r_algo_a].type == REG_INT) algo_a = (node_id_t)ctx->reg[r_algo_a].i;

    bool has_a = (algo_a != 0);

    Pipeline *pl_a = NULL;
    Pipeline *pl_b = NULL;

    /*
     * Load both pipelines before constructing the composite.
     *
     * This is deliberately metadata-driven:
     * the composer does not know what A or B actually do.
     */
    if (has_a) {
        if (algorithm_load(ctx->memory.txn, algo_a, &pl_a) != 0 || !pl_a)
            return VM_ERROR;
    }

    if (algorithm_load(ctx->memory.txn, algo_b, &pl_b) != 0 || !pl_b) {
        pipeline_free(pl_a);
        return VM_ERROR;
    }

    /*
     * Pipeline I/O contract.
     *
     * Explicit metadata wins.
     * Missing metadata preserves the old R0 convention.
     */
    uint32_t out_a = 0;
    uint32_t in_b  = 0;

    if (has_a && pl_a->out_count > 0)
        out_a = pl_a->out_regs[0];

    if (pl_b->in_count > 0)
        in_b = pl_b->in_regs[0];

    /*
     * Validate register numbers before putting them into OP_MOVE.
     *
     * in_regs/out_regs are uint8_t in the serialized format, but the
     * VM still has a finite register file.
     */
    if (out_a >= VM_MAX_REGISTERS || in_b >= VM_MAX_REGISTERS) {
        pipeline_free(pl_a);
        pipeline_free(pl_b);
        return VM_INVALID_REGISTER;
    }

    /*
     * Base sequence:
     *
     *   LOAD_CONST(A)
     *   EXEC_ALGORITHM(A)
     *   [MOVE(B.in, A.out)]
     *   LOAD_CONST(B)
     *   EXEC_ALGORITHM(B)
     *   HALT
     *
     * 3 mandatory instructions + 2 for A + optional MOVE.
     */
    uint32_t capacity = 3u + (has_a ? 2u : 0u) + (has_a && out_a != in_b ? 1u : 0u);

    Pipeline *p = pipeline_create_with_capacity(capacity);

    if (!p) {
        pipeline_free(pl_a);
        pipeline_free(pl_b);
        return VM_OUT_OF_MEMORY;
    }

    /*
     * Composite input contract = A input contract.
     *
     * If A is absent, the composite is simply B.
     */
    if (has_a) {
        p->in_count = pl_a->in_count;

        if (p->in_count > 8) {
            pipeline_free(p);
            pipeline_free(pl_a);
            pipeline_free(pl_b);
            return VM_ERROR;
        }

        memcpy(p->in_regs,
               pl_a->in_regs,
               p->in_count * sizeof(uint8_t));
    } else {
        p->in_count = pl_b->in_count;

        if (p->in_count > 8) {
            pipeline_free(p);
            pipeline_free(pl_a);
            pipeline_free(pl_b);
            return VM_ERROR;
        }

        memcpy(p->in_regs,
               pl_b->in_regs,
               p->in_count * sizeof(uint8_t));
    }

    /*
     * Composite output contract = B output contract.
     */
    p->out_count = pl_b->out_count;

    if (p->out_count > 8) {
        pipeline_free(p);
        pipeline_free(pl_a);
        pipeline_free(pl_b);
        return VM_ERROR;
    }

    memcpy(p->out_regs,
           pl_b->out_regs,
           p->out_count * sizeof(uint8_t));

    pipeline_free(pl_a);
    pipeline_free(pl_b);

    /*
     * Constants:
     *
     *   [A, B] when A exists
     *   [B]    when A is absent
     */
    uint32_t constant_count = has_a ? 2u : 1u;

    p->constants.int_consts =
        malloc(constant_count * sizeof(int64_t));

    if (!p->constants.int_consts) {
        pipeline_free(p);
        return VM_OUT_OF_MEMORY;
    }

    uint32_t ci = 0;
    uint32_t const_a = 0;
    uint32_t const_b = 0;

    if (has_a) {
        p->constants.int_consts[ci] = (int64_t)algo_a;
        const_a = ci++;
    }

    p->constants.int_consts[ci] = (int64_t)algo_b;
    const_b = ci++;

    p->constants.int_count = ci;

    /*
     * The composite executor uses one dedicated register only for
     * passing algorithm IDs into OP_EXEC_ALGORITHM.
     *
     * The actual data path is NOT tied to COMPOSE_REG.
     */
    #define VM_COMPOSITION_ALGO_REG (VM_MAX_REGISTERS - 2)

    if (VM_COMPOSITION_ALGO_REG >= VM_MAX_REGISTERS) {
        pipeline_free(p);
        return VM_INVALID_REGISTER;
    }

    bool build_ok = true;

    if (has_a) {
        Instruction load_a = {
            .operator_id = OP_LOAD_CONST,
            .arg = { VM_COMPOSITION_ALGO_REG, const_a }
        };

        Instruction exec_a = {
            .operator_id = OP_EXEC_ALGORITHM,
            .arg = { VM_COMPOSITION_ALGO_REG }
        };

        build_ok =
            build_ok &&
            (pipeline_add_instruction(p, &load_a) == VM_OK);

        build_ok =
            build_ok &&
            (pipeline_add_instruction(p, &exec_a) == VM_OK);

        /*
         * Generic register adapter.
         *
         * Example:
         *
         *   A.out = R0
         *   B.in  = R1
         *
         * becomes:
         *
         *   MOVE R1 <- R0
         *
         * No knowledge of A/B is required.
         */
        if (out_a != in_b) {
            Instruction bridge = {
                .operator_id = OP_MOVE,
                .arg = { in_b, out_a, 0, 0, 0, 0 }
            };

            build_ok =
                build_ok &&
                (pipeline_add_instruction(p, &bridge) == VM_OK);
        }
    }

    Instruction load_b = {
        .operator_id = OP_LOAD_CONST,
        .arg = { VM_COMPOSITION_ALGO_REG, const_b }
    };

    Instruction exec_b = {
        .operator_id = OP_EXEC_ALGORITHM,
        .arg = { VM_COMPOSITION_ALGO_REG }
    };

    Instruction halt_i = {
        .operator_id = OP_HALT
    };

    build_ok =
        build_ok &&
        (pipeline_add_instruction(p, &load_b) == VM_OK);

    build_ok =
        build_ok &&
        (pipeline_add_instruction(p, &exec_b) == VM_OK);

    build_ok =
        build_ok &&
        (pipeline_add_instruction(p, &halt_i) == VM_OK);

    if (!build_ok) {
        pipeline_free(p);
        return VM_ERROR;
    }

    /*
     * Persist the generated executable as a normal algorithm.
     */
    node_id_t new_algo_id = hyper_memory_new_id(ctx->hyper_mem);

    int rc = algorithm_save(ctx->memory.txn, new_algo_id, p);

    pipeline_free(p);

    if (rc != MDB_SUCCESS) {
        LOG_ERROR("[ZERO_SHOT] algorithm_save failed for composite id=%lu: %s", (unsigned long)new_algo_id, mdb_strerror(rc));
        return VM_ERROR;
    }

    LOG_REASONER(
        "[ZERO_SHOT] synthesized composite algorithm id=%lu "
        "(A=%lu%s, B=%lu, bridge=R%lu<-R%lu)",
        (unsigned long)new_algo_id,
        (unsigned long)algo_a,
        has_a ? "" : " [skipped]",
        (unsigned long)algo_b,
        (unsigned long)in_b,
        (unsigned long)out_a
    );

    ctx->reg[r_dst].type = REG_NODE;
    ctx->reg[r_dst].node = new_algo_id;

    return VM_OK;
}
