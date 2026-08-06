// runtime/ops/tool_ops.c
//
// Мост Cognitive Cycle (Pipeline/VM) <-> Execution/Tools (docs/14_Tools.md)
// и доступ к db.graph.properties из байткода. Решение "когда и с какими
// аргументами звать инструмент" целиком в LMDB-Pipeline; здесь только
// безопасные нативные примитивы.
#include <stdlib.h>
#include <string.h>

#include "runtime/logging/logging.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/vm_status.h"
#include "runtime/register/register.h"
#include "runtime/arena/arena.h"
#include "runtime/object/object.h"
#include "runtime/compiler/pipeline.h"
#include "storage/property/property.h"
#include "knowledge/knowledge_cache.h"
#include "execution/executor.h"

// OP_LOAD_STR: arg[0]=dst регистр, arg[1]=индекс в ConstantPool.str_consts[].
// OP_LOAD_CONST читает ТОЛЬКО int_consts (см. memory_ops.c) — для строк
// нужен отдельный опкод. Register.string — НЕЗАВЛАДЕЮЩИЙ StringView:
// память принадлежит Pipeline->constants и живёт минимум до конца
// vm_execute() текущего фрейма (pipeline_free вызывается ПОСЛЕ vm_destroy
// во всех местах кодовой базы — vm_pool.c, cognitive.c). vm_register_clear()
// не пытается освобождать REG_STRING (тот же контракт, что в ontology_test.c).
int vm_op_load_str(VMContext *ctx, const Instruction *ins) {
    uint32_t dst = ins->arg[0];
    uint32_t idx = ins->arg[1];

    if (dst >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;

    VMFrame *frame = &ctx->frames[ctx->frame];
    const Pipeline *pl = frame->pipeline;
    if (!pl || !pl->constants.str_consts || idx >= pl->constants.str_count)
        return VM_ERROR;

    vm_register_clear(ctx, &ctx->reg[dst]);
    ctx->reg[dst].type = REG_STRING;
    ctx->reg[dst].string.data = pl->constants.str_consts[idx].data;
    ctx->reg[dst].string.len  = pl->constants.str_consts[idx].len;
    return VM_OK;
}

// Временный allowlist (см. TODO.md "Интеграция с OS" — полноценная
// Capability/Permission-система пока не реализована).
static bool interpreter_allowed(const char *interp) {
    static const char *ALLOWED[] = { "/bin/sh", "/bin/bash", "/usr/bin/python3", NULL };
    for (int i = 0; ALLOWED[i]; i++)
        if (strcmp(interp, ALLOWED[i]) == 0) return true;
    return false;
}

// OP_TOOL_EXEC: arg[0]=dst статус (REG_INT exit code),
// arg[1]=регистр интерпретатора (REG_STRING), arg[2]=регистр команды
// (REG_STRING), arg[3]=dst регистр захваченного вывода
// (REG_OBJECT/OBJECT_STRING; >= VM_MAX_REGISTERS -> вывод отбрасывается).
//
// БЛОКИРУЮЩИЙ вызов. Безопасен ТОЛЬКО потому, что исполняется исключительно
// внутри изолированного pthread воркера vm_pool (vm_pool.c::vm_worker) —
// без IPC-клиентов и без db_writer в стеке вызовов.
int vm_op_tool_exec(VMContext *ctx, const Instruction *ins) {
    uint32_t r_status = ins->arg[0];
    uint32_t r_interp = ins->arg[1];
    uint32_t r_cmd    = ins->arg[2];
    uint32_t r_output = ins->arg[3];

    if (r_status >= VM_MAX_REGISTERS || r_interp >= VM_MAX_REGISTERS || r_cmd >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if (ctx->reg[r_interp].type != REG_STRING || ctx->reg[r_cmd].type != REG_STRING)
        return VM_INVALID_TYPE;

    const char *interpreter = ctx->reg[r_interp].string.data;
    const char *command     = ctx->reg[r_cmd].string.data;
    if (!interpreter || !command)
        return VM_INVALID_TYPE;

    if (!interpreter_allowed(interpreter)) {
        LOG_WARN("OP_TOOL_EXEC: interpreter '%s' rejected by allowlist", interpreter);
        return VM_INVALID_TYPE;
    }

    char *output = NULL;
    int exit_code = -1, term_signal = 0;
    char *argv[] = { (char *)command, NULL };

    int rc = executor_run_script_sync(interpreter, "-c", argv, &output, &exit_code, &term_signal);
    if (rc != 0) {
        free(output);
        LOG_ERROR("OP_TOOL_EXEC: executor_run_script_sync failed ('%s -c %s')", interpreter, command);
        return VM_ERROR;
    }

    vm_register_clear(ctx, &ctx->reg[r_status]);
    ctx->reg[r_status].type = REG_INT;
    ctx->reg[r_status].i = exit_code;

    if (r_output < VM_MAX_REGISTERS) {
        VMHandle h = vm_object_new(&ctx->arena, OBJECT_STRING);
        VMObject *obj = (h.index != UINT32_MAX) ? vm_object_get(&ctx->arena, h) : NULL;
        if (obj) {
            StringView *sv = malloc(sizeof(StringView));
            if (sv) {
                sv->data = output ? output : strdup("");
                sv->len  = (uint32_t)strlen(sv->data);
                obj->data = sv;
                output = NULL;   // владение перешло в arena (object.c::destroy_string)
            }
            vm_register_clear(ctx, &ctx->reg[r_output]);
            ctx->reg[r_output].type = REG_OBJECT;
            ctx->reg[r_output].handle = h;
        }
    }

    free(output);   // NULL, если владение уже передано arena выше
    return (term_signal != 0) ? VM_ERROR : VM_OK;
}

// OP_LOAD_PROPERTIES: arg[0]=регистр сущности (REG_NODE или REG_INT).
// Подгружает числовые/bool свойства узла в ctx->preloaded_properties для
// последующих OP_PROP_GET в этом же запуске. PROP_STRING в VM-кэш не
// попадает (CachedProperty хранит только int/float/bool) — строковые
// свойства читаются снаружи через IPC "get_property".
int vm_op_load_properties(VMContext *ctx, const Instruction *ins) {
    uint32_t entity_reg = ins->arg[0];
    if (entity_reg >= VM_MAX_REGISTERS)
        return VM_INVALID_REGISTER;
    if (ctx->reg[entity_reg].type != REG_NODE && ctx->reg[entity_reg].type != REG_INT)
        return VM_INVALID_TYPE;

    node_id_t node_id = (ctx->reg[entity_reg].type == REG_NODE)
        ? ctx->reg[entity_reg].node : (node_id_t)ctx->reg[entity_reg].i;

    int rc = knowledge_cache_load_properties(ctx, ctx->memory.txn, node_id);
    return (rc == MDB_SUCCESS) ? VM_OK : VM_ERROR;
}
