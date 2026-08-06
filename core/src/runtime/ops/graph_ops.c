// runtime/ops/graph_ops.c
//
// Гомоиконичный интерпретатор: программа = цепочка NeuroAtom, связанных
// через idx_causal_rev. Каждый шаг — один mdb_get() по PK (O(log N)),
// декодирование в Instruction, диспетчеризация через штатный
// operator_execute() (та же Native Dispatch Table, что и для статических
// Pipeline). НИКАКИХ новых LMDB-таблиц: control flow переиспользует индекс
// причинности, уже используемый OP_TRACE для объяснимости.
#include <string.h>
#include <stdint.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/vm/vm_param.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "runtime/ops/graph_encoding.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "runtime/logging/logging.h"

// arg[0] = регистр с id стартового атома-инструкции (REG_NODE/REG_INT)
// arg[1] = регистр с max_steps (REG_INT; <=0 -> VM_EVAL_GRAPH_DEFAULT_STEPS)
// arg[2] = регистр-приёмник финального VMStatus (REG_INT)
int vm_op_eval_graph(VMContext *ctx, const Instruction *ins) {
    uint32_t r_start  = ins->arg[0];
    uint32_t r_max    = ins->arg[1];
    uint32_t r_status = ins->arg[2];

    if (r_start >= VM_MAX_REGISTERS ||
        r_max >= VM_MAX_REGISTERS ||
        r_status >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    if (ctx->reg[r_start].type != REG_NODE &&
        ctx->reg[r_start].type != REG_INT)
        return VM_INVALID_TYPE;

    if (!ctx->hyper_mem || !ctx->memory.txn)
        return VM_ERROR;

    node_id_t current = (ctx->reg[r_start].type == REG_NODE)
        ? ctx->reg[r_start].node
        : (node_id_t)ctx->reg[r_start].i;

    uint32_t max_steps =
        (ctx->reg[r_max].type == REG_INT && ctx->reg[r_max].i > 0)
            ? (uint32_t)ctx->reg[r_max].i
            : VM_EVAL_GRAPH_DEFAULT_STEPS;

    int status = VM_OK;
    uint32_t steps = 0;

    while (current != 0 && steps < max_steps) {
        // Сбрасываем указатель перехода перед каждой инструкцией.
        ctx->graph_jmp_target = 0;

        MDB_val key = { sizeof(node_id_t), &current };
        MDB_val data;

        // O(log N): точечное чтение по первичному ключу dbi_atoms.
        if (mdb_get(ctx->memory.txn,
                    ctx->hyper_mem->dbi_atoms,
                    &key,
                    &data) != MDB_SUCCESS ||
            data.mv_size != sizeof(NeuroAtom)) {
            status = VM_NOT_FOUND;
            break;
        }

        NeuroAtom instr_atom;
        memcpy(&instr_atom, data.mv_data, sizeof(NeuroAtom));

        if (proc_kind(instr_atom.process_id) != PROC_KIND_INSTRUCTION) {
            LOG_WARN("OP_EVAL_GRAPH: atom %lu is not PROC_KIND_INSTRUCTION",
                     (unsigned long)current);
            status = VM_INVALID_TYPE;
            break;
        }

        OperatorID op_id = (OperatorID)(instr_atom.process_id & PROC_ID_MASK);

        uint32_t unpacked[6];
        graph_unpack_args(HYPER_GET_ID(instr_atom.args[0].raw), unpacked);

        if (op_id == OP_GLOAD_CONST) {
            uint32_t dst = unpacked[0];

            if (dst >= VM_MAX_REGISTERS) {
                status = VM_INVALID_REGISTER;
                break;
            }

            ctx->reg[dst].type = REG_INT;
            ctx->reg[dst].i =
                (int64_t)HYPER_GET_ID(instr_atom.args[1].raw);

        } else {
            const Operator *op = operator_find(op_id);

            if (!op) {
                status = VM_UNKNOWN_OPCODE;
                break;
            }

            Instruction decoded = {0};
            decoded.operator_id = op_id;
            memcpy(decoded.arg, unpacked, sizeof(unpacked));

            int rc = operator_execute(ctx, op, &decoded);

            if (rc != VM_OK) {
                bool soft =
                    (rc == VM_NOT_FOUND) &&
                    (instr_atom.valence < 0.0f);

                if (!soft) {
                    status = rc;
                    break;
                }
            }
        }

        node_id_t next = 0;

        // === ИНТЕЛЛЕКТУАЛЬНЫЙ CONTROL FLOW ===
        if (ctx->graph_jmp_target != 0) {
            // Инструкция запросила явный переход.
            next = ctx->graph_jmp_target;
        } else {
            // Линейное выполнение: следующий узел = потомок
            // текущего в idx_causal_rev.
            MDB_cursor *cur;

            if (mdb_cursor_open(ctx->memory.txn,
                                db.graph.hyper.idx_causal_rev,
                                &cur) == MDB_SUCCESS) {
                MDB_val k = { sizeof(node_id_t), &current };
                MDB_val v;

                if (mdb_cursor_get(cur, &k, &v, MDB_SET) == MDB_SUCCESS &&
                    v.mv_size == sizeof(node_id_t))
                    memcpy(&next, v.mv_data, sizeof(node_id_t));

                mdb_cursor_close(cur);
            }
        }

        if (next == current) {
            status = VM_ERROR; // защита от самопетли
            break;
        }

        current = next;
        steps++;
    }

    ctx->reg[r_status].type = REG_INT;
    ctx->reg[r_status].i = status;

    return VM_OK;
}

int vm_op_jge_graph(VMContext *ctx, const Instruction *ins) {
    uint32_t reg_a      = ins->arg[0];
    uint32_t reg_b      = ins->arg[1];
    uint32_t target_reg = ins->arg[2];

    if (reg_a >= VM_MAX_REGISTERS ||
        reg_b >= VM_MAX_REGISTERS ||
        target_reg >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    Register *ra = &ctx->reg[reg_a];
    Register *rb = &ctx->reg[reg_b];

    bool condition_met = false;

    if (ra->type == REG_FLOAT && rb->type == REG_FLOAT) {
        condition_met = (ra->f > rb->f);

    } else if (ra->type == REG_INT && rb->type == REG_INT) {
        condition_met = (ra->i > rb->i);

    } else if (ra->type == REG_FLOAT && rb->type == REG_INT) {
        condition_met = (ra->f > (double)rb->i);

    } else if (ra->type == REG_INT && rb->type == REG_FLOAT) {
        condition_met = ((double)ra->i > rb->f);
    }

    if (condition_met) {
        if (ctx->reg[target_reg].type == REG_NODE) {
            ctx->graph_jmp_target =
                ctx->reg[target_reg].node;

        } else if (ctx->reg[target_reg].type == REG_INT) {
            ctx->graph_jmp_target =
                (node_id_t)ctx->reg[target_reg].i;

        } else {
            return VM_INVALID_TYPE;
        }
    }

    return VM_OK;
}

// OP_GLOAD_CONST вызванный НЕ через eval_graph (обычный OP_CALL/диспетчер) —
// программная ошибка. Отказываем явно и громко вместо тихого нуля.
int vm_op_gload_const_stub(VMContext *ctx, const Instruction *ins) {
    (void)ctx; (void)ins;
    LOG_ERROR("OP_GLOAD_CONST invoked outside OP_EVAL_GRAPH — this opcode is "
              "only valid as a graph-instruction atom body.");
    return VM_ERROR;
}

// arg[0] = OperatorID новой инструкции (immediate)
// arg[1] = sp_base: 6 упакованных полей лежат в scratchpad[sp_base..sp_base+5]
// arg[2] = регистр с cause_id (id предыдущей инструкции цепочки; 0 = начало)
// arg[3] = регистр-приёмник id нового атома
// arg[4] = регистр с "широким" операндом (используется, если arg[5]!=0)
// arg[5] = has_wide (immediate 0/1)
int vm_op_assert_instruction(VMContext *ctx, const Instruction *ins) {
    OperatorID target_opcode = (OperatorID)ins->arg[0];
    uint32_t sp_base  = ins->arg[1];
    uint32_t r_cause  = ins->arg[2];
    uint32_t r_dst    = ins->arg[3];
    uint32_t r_wide   = ins->arg[4];
    uint32_t has_wide = ins->arg[5];

    if (r_cause >= VM_MAX_REGISTERS || r_dst >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if (has_wide && r_wide >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if (sp_base + 5 >= MAX_SCRATCHPAD)
        return VM_INVALID_REGISTER;
    if (!ctx->hyper_mem) return VM_ERROR;
    if (ctx->reg[r_cause].type != REG_INT && ctx->reg[r_cause].type != REG_NODE)
        return VM_INVALID_TYPE;

    uint32_t fields[6];
    for (int i = 0; i < 6; i++)
        fields[i] = (uint32_t)(ctx->scratchpad[sp_base + i].value & GRAPH_INSTR_FIELD_MASK);
    uint64_t packed = graph_pack_args(fields);

    node_id_t cause_id = (ctx->reg[r_cause].type == REG_NODE)
        ? ctx->reg[r_cause].node : (node_id_t)ctx->reg[r_cause].i;

    NeuroAtom instr = {0};
    instr.id = hyper_memory_new_id(ctx->hyper_mem);
    instr.process_id = proc_make((ko_id_t)target_opcode, PROC_KIND_INSTRUCTION);
    instr.args[0].raw = (ko_id_t)(packed & HYPER_VALUE_MASK) | HYPER_TYPE_INT;

    if (has_wide) {
        if (ctx->reg[r_wide].type == REG_NODE)
            instr.args[1].raw = (ko_id_t)ctx->reg[r_wide].node;
        else if (ctx->reg[r_wide].type == REG_INT)
            instr.args[1].raw = (ko_id_t)ctx->reg[r_wide].i;
        else
            return VM_INVALID_TYPE;
    }

    // Синтезированный код стартует с УМЕРЕННОЙ уверенностью и НИЗКИМ LTI —
    // он не переживёт decay-цикл, если Критик не поднимет confidence
    // (см. OP_ATOM_REINFORCE ниже) и MERGE_CTX его не подтвердит.
    instr.truth_mean       = 1.0f;
    instr.truth_confidence = 0.5f;
    instr.sti = 0.6f;
    instr.lti = 0.05f;
    instr.utility = 0.0f;
    instr.valence = 0.0f;
    instr.context_or_time_link = ctx->current_context;  // уважает текущий OP_SPAWN_CTX

    if (hyper_assert_with_cause(ctx->memory.txn, ctx->hyper_mem, &instr, cause_id) < 0)
        return VM_ERROR;

    ctx->reg[r_dst].type = REG_INT;
    ctx->reg[r_dst].i = (int64_t)instr.id;
    ctx->last_result_id = instr.id;
    return VM_OK;
}

// arg[0] = регистр с id атома, чью truth_confidence корректируем
// arg[1] = регистр REG_FLOAT delta в [-1.0, 1.0]
// EMA-обновление на месте — тот же паттерн, что storage/graph/graph.c::upsert_edge().
int vm_op_atom_reinforce(VMContext *ctx, const Instruction *ins) {
    uint32_t r_atom  = ins->arg[0];
    uint32_t r_delta = ins->arg[1];

    if (r_atom >= VM_MAX_REGISTERS || r_delta >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if ((ctx->reg[r_atom].type != REG_NODE && ctx->reg[r_atom].type != REG_INT) ||
        ctx->reg[r_delta].type != REG_FLOAT)
        return VM_INVALID_TYPE;
    if (!ctx->hyper_mem) return VM_ERROR;

    node_id_t atom_id = (ctx->reg[r_atom].type == REG_NODE)
        ? ctx->reg[r_atom].node : (node_id_t)ctx->reg[r_atom].i;
    float delta = (float)ctx->reg[r_delta].f;

    MDB_val key = { sizeof(node_id_t), &atom_id };
    MDB_val data;
    if (mdb_get(ctx->memory.txn, ctx->hyper_mem->dbi_atoms, &key, &data) != MDB_SUCCESS ||
        data.mv_size != sizeof(NeuroAtom))
        return VM_NOT_FOUND;

    NeuroAtom atom;
    memcpy(&atom, data.mv_data, sizeof(NeuroAtom));

    if (delta >= 0.0f)
        atom.truth_confidence += (1.0f - atom.truth_confidence) * delta;
    else
        atom.truth_confidence += atom.truth_confidence * delta;

    if (atom.truth_confidence < 0.0f) atom.truth_confidence = 0.0f;
    if (atom.truth_confidence > 1.0f) atom.truth_confidence = 1.0f;

    // hyper_assert() (не _unique) — перезапись на месте по неизменному id,
    // индексы не трогаются (process_id/args не меняются).
    if (hyper_assert(ctx->memory.txn, ctx->hyper_mem, &atom) != MDB_SUCCESS)
        return VM_ERROR;

    return VM_OK;
}
