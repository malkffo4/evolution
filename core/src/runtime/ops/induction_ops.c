// runtime/ops/induction_ops.c
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/logging/logging.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"

#define PATTERN_MINE_MAX_CHILDREN 128
#define PATTERN_MINE_MAX_GROUPS   32

// arg[0]=r_subject (REG_NODE/INT, узел A), arg[1]=r_min_count (REG_INT)
// arg[2]=r_proc_out (REG_INT, process_id самого частого паттерна)
// arg[3]=r_sample_out (REG_NODE, id одного из атомов паттерна)
// arg[4]=r_count_out (REG_INT), arg[5]=r_found_out (REG_INT 0/1)
int vm_op_mine_causal_pattern(VMContext *ctx, const Instruction *ins) {
    uint32_t r_subject   = ins->arg[0];
    uint32_t r_min_count = ins->arg[1];
    uint32_t r_proc_out  = ins->arg[2];
    uint32_t r_sample_out= ins->arg[3]; // Больше не нужен, но оставляем для совместимости подписи
    uint32_t r_count_out = ins->arg[4];
    uint32_t r_found_out = ins->arg[5];

    if (r_subject >= VM_MAX_REGISTERS || r_min_count >= VM_MAX_REGISTERS ||
        r_proc_out >= VM_MAX_REGISTERS || r_sample_out >= VM_MAX_REGISTERS ||
        r_count_out >= VM_MAX_REGISTERS || r_found_out >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if (!ctx->hyper_mem || !ctx->memory.txn) return VM_ERROR;

    node_id_t subject = (ctx->reg[r_subject].type == REG_NODE)
        ? ctx->reg[r_subject].node : (node_id_t)ctx->reg[r_subject].i;
    int32_t min_count = (ctx->reg[r_min_count].type == REG_INT)
        ? (int32_t)ctx->reg[r_min_count].i : 2;

    MDB_cursor *cur;
    if (mdb_cursor_open(ctx->memory.txn, db.graph.hyper.idx_causal_rev, &cur) != MDB_SUCCESS)
        return VM_ERROR;

    MDB_val key = { sizeof(node_id_t), &subject };
    MDB_val val;

    typedef struct { ko_id_t process_id; uint32_t count; } Group;
    Group groups[PATTERN_MINE_MAX_GROUPS] = {0};
    uint32_t group_count = 0;
    uint32_t scanned = 0;

    int rc = mdb_cursor_get(cur, &key, &val, MDB_SET);
    while (rc == MDB_SUCCESS && scanned < PATTERN_MINE_MAX_CHILDREN) {
        if (val.mv_size == sizeof(node_id_t)) {
            node_id_t child_id;
            memcpy(&child_id, val.mv_data, sizeof(node_id_t));
            scanned++;

            MDB_val k2 = { sizeof(node_id_t), &child_id };
            MDB_val d2;
            if (mdb_get(ctx->memory.txn, ctx->hyper_mem->dbi_atoms, &k2, &d2) == MDB_SUCCESS &&
                d2.mv_size == sizeof(NeuroAtom)) {
                NeuroAtom child;
                memcpy(&child, d2.mv_data, sizeof(NeuroAtom));

                ProcKind kind = proc_kind(child.process_id);
                if (kind != PROC_KIND_EVENT && kind != PROC_KIND_INSTRUCTION && kind != PROC_KIND_GOAL) {
                    bool found_group = false;
                    for (uint32_t g = 0; g < group_count; g++) {
                        if (groups[g].process_id == child.process_id) {
                            groups[g].count++;
                            found_group = true;
                            break;
                        }
                    }
                    if (!found_group && group_count < PATTERN_MINE_MAX_GROUPS) {
                        groups[group_count].process_id = child.process_id;
                        groups[group_count].count = 1;
                        group_count++;
                    }
                }
            }
        }
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT_DUP);
    }
    mdb_cursor_close(cur);

    uint32_t best = UINT32_MAX;
    for (uint32_t g = 0; g < group_count; g++) {
        if ((int32_t)groups[g].count < min_count) continue;
        if (best == UINT32_MAX || groups[g].count > groups[best].count)
            best = g;
    }

    if (best == UINT32_MAX) {
        LOG_REASONER("[MINE_CAUSAL_PATTERN] subject=%lu scanned=%u groups=%u -> no pattern >= min_count=%d",
                     (unsigned long)subject, scanned, group_count, min_count);
        ctx->reg[r_found_out].type = REG_INT;
        ctx->reg[r_found_out].i = 0;
        return VM_OK;
    }

    LOG_REASONER("[MINE_CAUSAL_PATTERN] subject=%lu scanned=%u groups=%u -> pattern process=%lu count=%u",
                 (unsigned long)subject, scanned, group_count,
                 (unsigned long)groups[best].process_id, groups[best].count);

    ctx->reg[r_proc_out].type = REG_INT;
    ctx->reg[r_proc_out].i = (int64_t)groups[best].process_id;
    ctx->reg[r_sample_out].type = REG_INT;
    ctx->reg[r_sample_out].i = 0;
    ctx->reg[r_count_out].type = REG_INT;
    ctx->reg[r_count_out].i = (int64_t)groups[best].count;
    ctx->reg[r_found_out].type = REG_INT;
    ctx->reg[r_found_out].i = 1;
    return VM_OK;
}
