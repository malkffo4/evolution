// runtime/ops/hyper_ops.c
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "storage/hyper_atom/hyper_atom.h"

#define ID_IS_CHILD_OF 0x0001
#define ID_BELIEF      0x0002

typedef struct { ko_id_t old_id; ko_id_t new_id; } IdMap;

// Читает truth_confidence из уже сохранённого атома (belief теперь читается
// напрямую из вектора truth, отдельный процесс ID_BELIEF больше не нужен
// для confidence — но оставлен для обратной совместимости с legacy данными).
/* static float get_atom_confidence(HyperMemory *mem, ko_id_t atom_id) {
    MDB_val key = { sizeof(ko_id_t), &atom_id };
    MDB_val data;
    if (mdb_get(txn, mem->dbi_atoms, &key, &data) == MDB_SUCCESS &&
        data.mv_size == sizeof(NeuroAtom)) {
        NeuroAtom *a = (NeuroAtom *)data.mv_data;
        return a->truth_confidence;
    }
    return 0.5f; // дефолт для несуществующего/повреждённого атома
} */

static ko_id_t get_parent_context(MDB_txn *txn, HyperMemory *mem, ko_id_t ctx_id) {
    if (ctx_id == 0) return 0;

    NeuroAtom *results = NULL;
    size_t count = 0;
    ko_id_t parent_id = 0;

    if (hyper_find_by_participant(txn, mem, ctx_id, 0, &results, &count) == 0) {
        for (size_t i = 0; i < count; i++) {
            if (proc_kind(results[i].process_id) != PROC_KIND_RELATION) continue;
            if ((results[i].process_id & PROC_ID_MASK) != ID_IS_CHILD_OF) continue;
            if (HYPER_GET_ID(results[i].args[0].raw) == ctx_id) {
                parent_id = HYPER_GET_ID(results[i].args[1].raw);
                break;
            }
        }
    }
    if (results) free(results);
    return parent_id;
}

// --- HYPER OPS: строго args[2] ---

// OP_QUERY: arg[0]=process_id_reg, arg[1]=participant_reg, arg[2]=context_reg -> sp[arg[3]], count->reg[arg[4]]
int vm_op_query(VMContext *ctx, const Instruction *ins) {
    ko_id_t proc_id     = (ko_id_t)ctx->reg[ins->arg[0]].i;
    ko_id_t participant = (ko_id_t)ctx->reg[ins->arg[1]].i;
    ko_id_t context      = (ko_id_t)ctx->reg[ins->arg[2]].i;
    uint32_t sp_offset  = ins->arg[3];

    // Опциональный STI-порог из регистра (если arg[5] != 0)
    float sti_threshold = (ins->arg[5] != 0) ? (float)ctx->reg[ins->arg[5]].f : 0.0f;

    NeuroAtom *results = NULL;
    size_t count = 0;

    if (sti_threshold > 0.0f) {
        hyper_find_by_process_sti(ctx->memory.txn, ctx->hyper_mem, proc_id, participant, context, sti_threshold, &results, &count);
    } else {
        hyper_find_by_process(ctx->memory.txn, ctx->hyper_mem, proc_id, participant, context, &results, &count);
    }

    for (size_t i = 0; i < count && (sp_offset + i) < MAX_SCRATCHPAD; i++)
        ctx->scratchpad[sp_offset + i].value = (int64_t)results[i].id;

    ctx->reg[ins->arg[4]].type = REG_INT;
    ctx->reg[ins->arg[4]].i = (int64_t)count;

    if (results) free(results);
    return VM_OK;
}

// OP_ASSERT: arg[0]=process_reg, arg[1..2]=args0,args1 regs, arg[3]=dst_id_reg
// Новый факт наследует дефолтные значения когнитивной триады (см. п.2 TASK 1):
// truth высок (это прямое утверждение), attention начальная, utility/valence нейтральны.
int vm_op_assert(VMContext *ctx, const Instruction *ins) {
    NeuroAtom atom = {0};
    atom.id = hyper_memory_new_id(ctx->hyper_mem);
    atom.process_id = (ko_id_t)ctx->reg[ins->arg[0]].i;

    atom.args[0].raw = (ko_id_t)ctx->reg[ins->arg[1]].i;
    atom.args[1].raw = (ko_id_t)ctx->reg[ins->arg[2]].i;

    atom.truth_mean = 1.0f;
    atom.truth_confidence = 0.6f;   // прямое ASSERT чуть увереннее дефолта
    atom.sti = 0.7f;                // свежий факт — в фокусе внимания
    atom.lti = 0.1f;
    atom.utility = 0.0f;
    atom.valence = 0.0f;

    atom.context_or_time_link = ctx->current_context;

    // Причина ASSERT'а (не DERIVE) — текущий эпизод, через idx_causal
    if (hyper_assert_with_cause(ctx->memory.txn, ctx->hyper_mem, &atom, ctx->current_episode_id) < 0)
        return VM_ERROR;

    ctx->reg[ins->arg[3]].type = REG_INT;
    ctx->reg[ins->arg[3]].i = (int64_t)atom.id;
    ctx->last_result_id = atom.id;
    return VM_OK;
}

// OP_DERIVE: как ASSERT, но cause_id берётся из регистра (arg[3]) — логический вывод.
// arg[0]=process, arg[1..2]=args, arg[3]=cause_id_reg, arg[4]=dst_id_reg
int vm_op_derive(VMContext *ctx, const Instruction *ins) {
    NeuroAtom atom = {0};
    atom.id = hyper_memory_new_id(ctx->hyper_mem);
    atom.process_id = (ko_id_t)ctx->reg[ins->arg[0]].i;

    atom.args[0].raw = (ko_id_t)ctx->reg[ins->arg[1]].i;
    atom.args[1].raw = (ko_id_t)ctx->reg[ins->arg[2]].i;

    // Выведенное знание изначально менее уверенно, чем прямой ASSERT —
    // confidence зависит от источника (можно передавать через доп. регистр).
    atom.truth_mean = 1.0f;
    atom.truth_confidence = 0.4f;
    atom.sti = 0.5f;
    atom.lti = 0.05f;
    atom.utility = 0.0f;
    atom.valence = 0.0f;

    atom.context_or_time_link = ctx->current_context;

    ko_id_t cause_id = (ko_id_t)ctx->reg[ins->arg[3]].i;

    if (hyper_assert_with_cause(ctx->memory.txn, ctx->hyper_mem, &atom, cause_id) < 0)
        return VM_ERROR;

    ctx->reg[ins->arg[4]].type = REG_INT;
    ctx->reg[ins->arg[4]].i = (int64_t)atom.id;
    ctx->last_result_id = atom.id;
    return VM_OK;
}

// OP_TRACE: обходит idx_causal вместо поля atom->cause_id
// arg[0]=start_id_reg, arg[1]=max_depth(imm), arg[2]=sp_offset, arg[3]=count_reg
int vm_op_trace(VMContext *ctx, const Instruction *ins) {
    ko_id_t current_id = (ko_id_t)ctx->reg[ins->arg[0]].i;
    uint32_t max_depth = ins->arg[1];
    uint32_t sp_offset = ins->arg[2];

    uint32_t count = 0;
    for (uint32_t depth = 0; depth < max_depth && current_id != 0 &&
         (sp_offset + count) < MAX_SCRATCHPAD; depth++) {

        ctx->scratchpad[sp_offset + count].value = (int64_t)current_id;
        count++;

        // Ищем родителя в idx_causal: child_id -> cause_id
        MDB_val key = { sizeof(ko_id_t), &current_id };
        MDB_val val;
        MDB_cursor *cur;
        ko_id_t next_id = 0;
        if (mdb_cursor_open(ctx->memory.txn, ctx->hyper_mem->dbi_idx_causal, &cur) == MDB_SUCCESS) {
            if (mdb_cursor_get(cur, &key, &val, MDB_SET) == MDB_SUCCESS && val.mv_size == sizeof(ko_id_t)) {
                memcpy(&next_id, val.mv_data, sizeof(ko_id_t));
            }
            mdb_cursor_close(cur);
        }
        current_id = next_id;
    }

    ctx->reg[ins->arg[3]].type = REG_INT;
    ctx->reg[ins->arg[3]].i = (int64_t)count;
    return VM_OK;
}

// OP_SPAWN_CTX — без изменений структурно, args[2] не используются здесь напрямую
int vm_op_spawn_ctx(VMContext *ctx, const Instruction *ins) {
    ko_id_t child_id = hyper_memory_new_id(ctx->hyper_mem);

    NeuroAtom rel = {0};
    rel.id = hyper_memory_new_id(ctx->hyper_mem);
    rel.process_id = proc_make(ID_IS_CHILD_OF, PROC_KIND_RELATION);
    rel.args[0].raw = HYPER_MAKE_REF(child_id);
    rel.args[1].raw = HYPER_MAKE_REF(ctx->current_context);
    rel.truth_mean = 1.0f;
    rel.truth_confidence = 1.0f;
    rel.sti = 0.3f;
    rel.context_or_time_link = 0;

    hyper_assert_with_cause(ctx->memory.txn, ctx->hyper_mem, &rel, ctx->current_episode_id);

    ctx->current_context = child_id;

    ctx->reg[ins->arg[0]].type = REG_INT;
    ctx->reg[ins->arg[0]].i = (int64_t)child_id;
    return VM_OK;
}

// Вспомогательная функция: ремап причинного индекса
static void remap_causal_index(MDB_txn *txn, HyperMemory *hmem, const IdMap *id_map, size_t map_size) {
    if (!hmem->dbi_idx_causal) return;

    MDB_cursor *cur;
    if (mdb_cursor_open(txn, hmem->dbi_idx_causal, &cur) != MDB_SUCCESS)
        return;

    for (size_t m = 0; m < map_size; m++) {
        ko_id_t old_id = id_map[m].old_id;
        ko_id_t new_id = id_map[m].new_id;

        // --- old_id как child (причина для других) ---
        MDB_val key = { sizeof(ko_id_t), &old_id };
        MDB_val val;
        if (mdb_cursor_get(cur, &key, &val, MDB_SET) == MDB_SUCCESS) {
            do {
                MDB_val cause_val = val;  // копируем значение (cause_id)
                // Вставляем запись с new_id
                key.mv_data = &new_id;
                mdb_put(txn, hmem->dbi_idx_causal, &key, &cause_val, MDB_APPENDDUP);
                // Удаляем старую запись (курсор всё ещё на old_id)
                mdb_cursor_del(cur, 0);
            } while (mdb_cursor_get(cur, &key, &val, MDB_NEXT_DUP) == MDB_SUCCESS);
        }

        // --- old_id как parent (следствие для других) ---
        // Ищем все записи, где в значении (mv_data) указан old_id.
        // Поскольку DUPSORT, мы не можем искать по значению напрямую.
        // Приходится сканировать весь индекс, но только один раз за merge – приемлемо.
        MDB_val scan_key, scan_val;
        if (mdb_cursor_get(cur, &scan_key, &scan_val, MDB_FIRST) == MDB_SUCCESS) {
            do {
                if (scan_val.mv_size == sizeof(ko_id_t)) {
                    ko_id_t cause = *(ko_id_t*)scan_val.mv_data;
                    if (cause == old_id) {
                        ko_id_t child = *(ko_id_t*)scan_key.mv_data;
                        // Удаляем старую пару (child, old_id)
                        mdb_cursor_del(cur, 0);
                        // Добавляем новую пару (child, new_id)
                        MDB_val new_key = { sizeof(ko_id_t), &child };
                        MDB_val new_val = { sizeof(ko_id_t), &new_id };
                        mdb_put(txn, hmem->dbi_idx_causal, &new_key, &new_val, MDB_APPENDDUP);
                        // Перезапускаем курсор на FIRST, т.к. мы изменили данные
                        mdb_cursor_get(cur, &scan_key, &scan_val, MDB_FIRST);
                        continue;
                    }
                }
            } while (mdb_cursor_get(cur, &scan_key, &scan_val, MDB_NEXT) == MDB_SUCCESS);
        }
    }

    mdb_cursor_close(cur);
}

// OP_MERGE_CTX: схлопывание гипотезы. Теперь ремапит id и в idx_causal тоже.
int vm_op_merge_ctx(VMContext *ctx, const Instruction *ins) {
    float threshold = *(float*)&ins->arg[0];

    NeuroAtom *atoms = NULL;
    size_t count = 0;

    if (hyper_find_by_process(ctx->memory.txn, ctx->hyper_mem, 0, 0, ctx->current_context, &atoms, &count) != 0)
        return VM_ERROR;

    ko_id_t parent = get_parent_context(ctx->memory.txn, ctx->hyper_mem, ctx->current_context);

    IdMap *id_map = count > 0 ? malloc(sizeof(IdMap) * count) : NULL;
    if (!id_map && count > 0) {
        if (atoms) free(atoms);
        return VM_ERROR;
    }
    size_t map_size = 0;

    for (size_t i = 0; i < count; i++) {
        float conf = atoms[i].truth_confidence;
        if (conf >= threshold) {
            ko_id_t new_id = hyper_memory_new_id(ctx->hyper_mem);
            id_map[map_size].old_id = atoms[i].id;
            id_map[map_size].new_id = new_id;
            map_size++;

            atoms[i].id = new_id;
            atoms[i].context_or_time_link = parent;
        } else {
            atoms[i].id = 0;
        }
    }

    // **РЕМАП ПРИЧИННОСТИ И ИНДЕКСОВ**
    if (map_size > 0) {
        remap_causal_index(ctx->memory.txn, ctx->hyper_mem, id_map, map_size);

        // --- НОВЫЙ БЛОК: Ремаппинг ссылок внутри самой базы атомов и индексов ---
        for (size_t i = 0; i < count; i++) {
            if (atoms[i].id == 0) continue;

            // Ремапим ссылки-аргументы (args[0], args[1]) внутри самого атома
            for (int arg_idx = 0; arg_idx < HYPER_VAL_SLOTS; arg_idx++) {
                if (HYPER_GET_TYPE(atoms[i].args[arg_idx].raw) == HYPER_TYPE_REF) {
                    ko_id_t ref_val = HYPER_GET_ID(atoms[i].args[arg_idx].raw);
                    for (size_t m = 0; m < map_size; m++) {
                        if (ref_val == id_map[m].old_id) {
                            atoms[i].args[arg_idx].raw = HYPER_MAKE_REF(id_map[m].new_id);
                            break;
                        }
                    }
                }
            }

            // Фиксируем обновленный атом в базе
            hyper_assert_unique(ctx->memory.txn, ctx->hyper_mem, &atoms[i]);
        }
    }

    if (id_map) free(id_map);
    if (atoms) free(atoms);

    ctx->current_context = parent;
    return VM_OK;
}
