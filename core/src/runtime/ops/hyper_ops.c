// runtime/ops/hyper_ops.c
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "storage/hyper_atom/hyper_atom.h"

// Системные константы AGI Meta-Core
#define ID_IS_CHILD_OF 0x0001
#define ID_BELIEF      0x0002

// --- РЕАЛИЗАЦИЯ ХЕЛПЕРОВ СИСТЕМЫ ПРИЧИННОСТИ ---

static ko_id_t generate_id(VMContext *ctx) {
    static uint64_t counter = 1;
    // Смешиваем время (старшие 32 бита) и инкремент (младшие 32 бита).
    // Это предотвратит коллизии ID даже если VM перезапустится.
    uint64_t t = (uint64_t)time(NULL);
    return (t << 32) | (counter++ & 0xFFFFFFFF);
}

static uint64_t get_time_tick(VMContext *ctx) {
    // Возвращаем реальное логическое время виртуальной машины
    return ctx->cycles;
}

static float get_confidence(HyperMemory *mem, ko_id_t atom_id) {
    HyperAtom *results = NULL;
    size_t count = 0;
    float confidence = 1.0f; // По умолчанию доверие полное (1.0), если не указано иное

    // Ищем все процессы, в которых участвует наш атом (ищем оценку)
    if (hyper_find_by_participant(mem, atom_id, 0, &results, &count) == 0) {
        for (size_t i = 0; i < count; i++) {
            if (results[i].process_id == ID_BELIEF) {
                // Если процесс - ID_BELIEF, где первый аргумент это наш атом,
                // то второй аргумент содержит float-значение уверенности.
                if (HYPER_GET_ID(results[i].args[0].raw) == atom_id) {
                    confidence = (float)results[i].args[1].f_val;
                    break;
                }
            }
        }
    }

    if (results) free(results);
    return confidence;
}

static ko_id_t get_parent_context(HyperMemory *mem, ko_id_t ctx_id) {
    if (ctx_id == 0) return 0; // Базовая реальность всегда корень

    HyperAtom *results = NULL;
    size_t count = 0;
    ko_id_t parent_id = 0;

    // Ищем связь [ctx_id] -> [IS_CHILD_OF] -> [parent_id] в базовой реальности
    if (hyper_find_by_participant(mem, ctx_id, 0, &results, &count) == 0) {
        for (size_t i = 0; i < count; i++) {
            if (results[i].process_id == ID_IS_CHILD_OF) {
                if (HYPER_GET_ID(results[i].args[0].raw) == ctx_id) {
                    parent_id = HYPER_GET_ID(results[i].args[1].raw);
                    break;
                }
            }
        }
    }

    if (results) free(results);
    return parent_id;
}

// --- HYPER OPS ИМПЛЕМЕНТАЦИИ ---

// OP_QUERY: arg[0]=process_id, arg[1]=participant, arg[2]=context -> sp[arg[3]]
int vm_op_query(VMContext *ctx, const Instruction *ins) {
    ko_id_t proc_id     = (ko_id_t)ctx->reg[ins->arg[0]].i;
    ko_id_t participant = (ko_id_t)ctx->reg[ins->arg[1]].i;
    ko_id_t context     = (ko_id_t)ctx->reg[ins->arg[2]].i;
    uint32_t sp_offset  = ins->arg[3];

    HyperAtom *results = NULL;
    size_t count = 0;

    if (participant != 0)
        hyper_find_by_participant(ctx->hyper_mem, participant, context, &results, &count);
    else
        hyper_find_by_process(ctx->hyper_mem, proc_id, context, &results, &count);

    // Сохраняем ID найденных атомов в scratchpad
    for (size_t i = 0; i < count && (sp_offset + i) < MAX_SCRATCHPAD; i++)
        ctx->scratchpad[sp_offset + i].value = (int64_t)results[i].id;

    // В следующий регистр (arg[4]) запишем количество результатов
    ctx->reg[ins->arg[4]].type = REG_INT;
    ctx->reg[ins->arg[4]].i = (int64_t)count;

    if (results) free(results);

    return VM_OK;
}

// OP_ASSERT: Создает атом в текущем контексте, cause_id = текущий эпизод
int vm_op_assert(VMContext *ctx, const Instruction *ins) {
    HyperAtom atom = {0};
    atom.id = generate_id(ctx);
    atom.process_id = (ko_id_t)ctx->reg[ins->arg[0]].i;

    for (int i = 0; i < 3; i++) {
        atom.args[i].raw = ctx->reg[ins->arg[1+i]].i;
    }

    atom.context_id = ctx->current_context;
    atom.time_tick = get_time_tick(ctx);
    atom.cause_id = ctx->current_episode_id;

    if (hyper_assert_unique(ctx->hyper_mem, &atom) != 0) return VM_ERROR;

    ctx->reg[ins->arg[4]].type = REG_INT;
    ctx->reg[ins->arg[4]].i = (int64_t)atom.id;

    return VM_OK;
}

// OP_DERIVE: Как ASSERT, но cause_id берется из регистра (логический вывод)
int vm_op_derive(VMContext *ctx, const Instruction *ins) {
    HyperAtom atom = {0};
    atom.id = generate_id(ctx);
    atom.process_id = (ko_id_t)ctx->reg[ins->arg[0]].i;

    for (int i = 0; i < 3; i++) {
        atom.args[i].raw = ctx->reg[ins->arg[1+i]].i;
    }

    atom.context_id = ctx->current_context;
    atom.time_tick = get_time_tick(ctx);
    atom.cause_id = (ko_id_t)ctx->reg[ins->arg[4]].i;

    if (hyper_assert_unique(ctx->hyper_mem, &atom) != 0) return VM_ERROR;

    ctx->reg[ins->arg[5]].type = REG_INT;
    ctx->reg[ins->arg[5]].i = (int64_t)atom.id;

    return VM_OK;
}

// OP_TRACE: проходит по cause_id и складывает цепочку в scratchpad
int vm_op_trace(VMContext *ctx, const Instruction *ins) {
    ko_id_t start_id = ctx->reg[ins->arg[0]].i;
    size_t depth = ins->arg[1];
    uint32_t sp_offset = ins->arg[2];

    HyperAtom *chain = NULL;
    size_t count = 0;

    hyper_trace_cause(ctx->hyper_mem, start_id, &chain, depth, &count);

    for (size_t i = 0; i < count && (sp_offset + i) < MAX_SCRATCHPAD; i++) {
        ctx->scratchpad[sp_offset + i].value = (int64_t)chain[i].id;
    }

    ctx->reg[ins->arg[3]].type = REG_INT;
    ctx->reg[ins->arg[3]].i = (int64_t)count;

    if (chain) free(chain);
    return VM_OK;
}

// OP_SPAWN_CTX: Создает ветку реальности
int vm_op_spawn_ctx(VMContext *ctx, const Instruction *ins) {
    ko_id_t child_id = generate_id(ctx);

    HyperAtom rel = {0};
    rel.id = generate_id(ctx);
    rel.process_id = ID_IS_CHILD_OF;
    rel.args[0].raw = HYPER_MAKE_REF(child_id);
    rel.args[1].raw = HYPER_MAKE_REF(ctx->current_context);
    rel.context_id = 0; // Мета-отношения пишутся в базовую реальность
    rel.time_tick = get_time_tick(ctx);
    rel.cause_id = ctx->current_episode_id;

    hyper_assert_unique(ctx->hyper_mem, &rel);

    ctx->current_context = child_id;

    ctx->reg[ins->arg[0]].type = REG_INT;
    ctx->reg[ins->arg[0]].i = (int64_t)child_id;

    return VM_OK;
}

// OP_MERGE_CTX: Схлопывание гипотезы (возврат в реальность)
int vm_op_merge_ctx(VMContext *ctx, const Instruction *ins) {
    float threshold = *(float*)&ins->arg[0]; // Читаем порог из аргумента-поплавка

    HyperAtom *atoms = NULL;
    size_t count = 0;

    // Выгружаем ВСЕ атомы текущей гипотезы
    if (hyper_find_by_process(ctx->hyper_mem, 0, ctx->current_context, &atoms, &count) != 0) {
        return VM_ERROR; // Не удалось выгрузить контекст
    }

    ko_id_t parent = get_parent_context(ctx->hyper_mem, ctx->current_context);

    // Таблица маппинга старых ID (внутри гипотезы) в новые ID (в реальности)
    typedef struct { ko_id_t old_id; ko_id_t new_id; } IdMap;
    IdMap *id_map = count > 0 ? malloc(sizeof(IdMap) * count) : NULL;
    size_t map_size = 0;

    // --- ПЕРВЫЙ ПРОХОД: Фильтрация и генерация новых ID ---
    for (size_t i = 0; i < count; i++) {
        float conf = get_confidence(ctx->hyper_mem, atoms[i].id);
        if (conf >= threshold) {
            ko_id_t new_id = generate_id(ctx);
            id_map[map_size].old_id = atoms[i].id;
            id_map[map_size].new_id = new_id;
            map_size++;

            // Сразу меняем ID и контекст в массиве
            atoms[i].id = new_id;
            atoms[i].context_id = parent;
        } else {
            // Помечаем нулем те атомы, которые не прошли порог уверенности
            atoms[i].id = 0;
        }
    }

    // --- ВТОРОЙ ПРОХОД: Перемапливание cause_id и ссылок (граф внутри гипотезы) ---
    for (size_t i = 0; i < count; i++) {
        if (atoms[i].id == 0) continue; // Атом не переносится

        // 1. Исправляем cause_id, если причина также перенеслась из гипотезы
        for (size_t m = 0; m < map_size; m++) {
            if (atoms[i].cause_id == id_map[m].old_id) {
                atoms[i].cause_id = id_map[m].new_id;
                break;
            }
        }

        // 2. ОЧЕНЬ ВАЖНО: исправляем аргументы-ссылки.
        // Если внутри гипотезы был создан новый объект и тут же использован!
        for (int arg_idx = 0; arg_idx < 3; arg_idx++) {
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

        // Сохраняем "материализованный" факт в родительский контекст
        hyper_assert_unique(ctx->hyper_mem, &atoms[i]);
    }

    if (id_map) free(id_map);
    if (atoms) free(atoms);

    // Возвращаемся в родительский контекст
    ctx->current_context = parent;

    return VM_OK;
}
