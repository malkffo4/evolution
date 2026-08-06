// ipc/handlers/command/cmd_execute.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "ipc/ipc.h"
#include "storage/db/db.h"
#include "storage/db/db_writer.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "storage/string_pool/string_pool.h"
#include "memory/working.h"
#include "core/globals.h"
#include "math/hash.h"
#include "runtime/logging/logging.h"
#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/ops/opcode.h"
#include "runtime/compiler/pipeline.h"
#include "knowledge/evaluation.h"

typedef struct {
    char      goal_name[256];
    float     utility;
    node_id_t goal_id; // выход
} GoalActivationJob;

typedef struct {
    node_id_t algo_id;
    struct { uint32_t reg; int64_t value; } init_regs[8];
    int        init_reg_count;
    uint32_t   report_regs[8];
    int        report_reg_count;
    int64_t    reported_values[8];
    int        vm_status;
} ExecJob;

static int exec_algorithm_txn_fn(MDB_txn *txn, void *arg) {
    ExecJob *job = arg;

    WorkingMemory local_wm;
    if (wm_init(&local_wm, 32) != 0) return -1;

    HyperMemory *hmem = hyper_memory_new(db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    if (!hmem) { wm_clear(&local_wm); return -1; }
    hyper_memory_set_db_causal(hmem, db.graph.hyper.idx_causal);

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (vm_init(&ctx, txn, &local_wm) != VM_OK) {
        hyper_memory_free(hmem); wm_clear(&local_wm); return -1;
    }
    ctx.hyper_mem = hmem;

    // Инъекция входных данных ("новых данных" из формулировки задачи) —
    // единственный способ передать runtime-параметры уже скомпилированному
    // алгоритму, чей байткод и константы зафиксированы заранее.
    for (int i = 0; i < job->init_reg_count; i++) {
        uint32_t r = job->init_regs[i].reg;
        if (r >= VM_MAX_REGISTERS) continue;
        ctx.reg[r].type = REG_INT;
        ctx.reg[r].i = job->init_regs[i].value;
    }

    ctx.reg[5].type = REG_INT;
    ctx.reg[5].i = (int64_t)job->algo_id;
    Instruction call = { .operator_id = OP_EXEC_ALGORITHM, .arg[0] = 5 };
    Pipeline outer = { .code = &call, .code_len = 1, .capacity = 1 };

    int rc = vm_execute(&ctx, &outer);
    job->vm_status = rc;

    for (int i = 0; i < job->report_reg_count; i++) {
        uint32_t r = job->report_regs[i];
        job->reported_values[i] =
            (r < VM_MAX_REGISTERS && ctx.reg[r].type == REG_INT) ? ctx.reg[r].i : 0;
    }

    // float outcome = (rc == VM_OK) ? 1.0f : 0.0f;
    // score_update(hmem, COGNITIVE_DOMAIN_ALGORITHM, job->algo_id, outcome, 0, 0);

    vm_destroy(&ctx);
    hyper_memory_free(hmem);
    wm_clear(&local_wm);
    return 0;
}

// Выполняется ИСКЛЮЧИТЕЛЬНО внутри write-транзакции потока db_writer.
// IPC-поток никогда не открывает собственную write-транзакцию — это
// гарантирует единственность писателя LMDB на весь процесс.
static int activate_goal_txn_fn(MDB_txn *txn, void *arg) {
    GoalActivationJob *job = arg;

    node_id_t goal_id = djb2_hash(job->goal_name);
    job->goal_id = goal_id;

    // Регистрируем читаемое имя цели в строковом пуле — используется позже
    // Research Engine'ом через get_string_from_pool() при VM_NOT_FOUND
    // (см. runtime/ops/cognitive.c::vm_op_evaluate_goals).
    add_string_to_pool(txn, job->goal_name);

    HyperMemory *hmem = hyper_memory_new(db.graph.hyper.atoms,
        db.graph.hyper.idx_process,
        db.graph.hyper.idx_args,
        db.graph.hyper.idx_context);
    if (!hmem) return -1;
    hyper_memory_set_db_causal(hmem, db.graph.hyper.idx_causal);

    // Knowledge Object минимально должен иметь тип
    // (docs/03_Knowledge.md: "Тип определяет семантику объекта").
    NeuroAtom type_atom = {0};
    type_atom.id          = hyper_memory_new_id(hmem);
    type_atom.process_id  = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
    type_atom.args[0].raw = HYPER_MAKE_REF(goal_id);
    type_atom.args[1].raw = HYPER_MAKE_REF(djb2_hash("Goal"));
    type_atom.truth_mean       = 1.0f;
    type_atom.truth_confidence = 1.0f;
    type_atom.sti = 0.5f;
    type_atom.lti = 0.2f;
    int rc = hyper_assert_unique(txn, hmem, &type_atom);
    hyper_memory_free(hmem);
    if (rc < 0) return -1;

    // Активация Global Workspace — единственное место, где IPC касается
    // WorkingMemory. Само рассуждение выполнит MainLoop асинхронно.
    wm_activate(&global_wm, goal_id, 1.0f, job->utility);
    wm_wrlock(&global_wm);
    for (uint32_t i = 0; i < global_wm.count; i++) {
        if (global_wm.nodes[i].node_id == goal_id) {
            global_wm.nodes[i].state.usefulness = job->utility;
            break;
        }
    }
    wm_unlock(&global_wm);

    return 0;
}

void cmd_execute_op(IPCPacket *req, IPCPacket *resp) {
    cJSON *root = cJSON_Parse((const char *)req->payload);
    if (!root) {
        resp->type = IPC_RESPONSE;
        strncpy(resp->name, "error", sizeof(resp->name) - 1);
        const char *err = "{\"error\": \"invalid JSON payload\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(resp->payload);
        return;
    }
    cJSON *op_json = cJSON_GetObjectItem(root, "op");

    // ИСПРАВЛЕНИЕ: Проверяем операцию ДО удаления root!
    if (cJSON_IsString(op_json) && strcmp(op_json->valuestring, "exec_algorithm") == 0) {
        ExecJob exec_job = {0};
        cJSON *regs = cJSON_GetObjectItem(root, "regs");
        if (cJSON_IsObject(regs)) {
            cJSON *item;
            cJSON_ArrayForEach(item, regs) {
                if (exec_job.init_reg_count >= 8) break;
                uint32_t reg_idx = (uint32_t)atoi(item->string);
                int64_t val = cJSON_IsString(item)
                    ? (int64_t)djb2_hash(item->valuestring)
                    : (int64_t)item->valuedouble;
                exec_job.init_regs[exec_job.init_reg_count].reg = reg_idx;
                exec_job.init_regs[exec_job.init_reg_count].value = val;
                if (reg_idx == 5) exec_job.algo_id = (node_id_t)val;
                exec_job.init_reg_count++;
            }
        }
        cJSON *report = cJSON_GetObjectItem(root, "report_regs");
        if (cJSON_IsArray(report)) {
            int n = cJSON_GetArraySize(report);
            for (int i = 0; i < n && i < 8; i++) {
                cJSON *arr_item = cJSON_GetArrayItem(report, i);
                exec_job.report_regs[exec_job.report_reg_count++] = (uint32_t)arr_item->valueint;
            }
        }
        cJSON_Delete(root); // Удаляем безопасно после считывания всех полей

        resp->type = IPC_RESPONSE;
        strncpy(resp->name, "execute_op", sizeof(resp->name) - 1);

        if (exec_job.algo_id == 0) {
            const char *err = "{\"error\": \"missing regs['5']\"}";
            strncpy(resp->payload, err, sizeof(resp->payload) - 1);
            resp->payload_size = (uint32_t)strlen(err);
            return;
        }

        db_write_sync(exec_algorithm_txn_fn, &exec_job);

        // ИСПРАВЛЕНИЕ: Реализуем честную сериализацию результатов
        cJSON *res_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(res_json, "vm_status", exec_job.vm_status);
        cJSON *regs_json = cJSON_CreateObject();
        for (int i = 0; i < exec_job.report_reg_count; i++) {
            char reg_str[16];
            snprintf(reg_str, sizeof(reg_str), "%u", exec_job.report_regs[i]);
            cJSON_AddNumberToObject(regs_json, reg_str, (double)exec_job.reported_values[i]);
        }
        cJSON_AddItemToObject(res_json, "reported_regs", regs_json);
        char *json_str = cJSON_PrintUnformatted(res_json);
        snprintf(resp->payload, sizeof(resp->payload), "%s", json_str);
        resp->payload_size = (uint32_t)strlen(resp->payload);
        free(json_str);
        cJSON_Delete(res_json);
        return;
    }

    // Fallback: Активация цели
    GoalActivationJob job = {0};
    job.utility = 0.9f;

    cJSON *goal_json = cJSON_GetObjectItem(root, "goal");
    if (cJSON_IsString(goal_json) && goal_json->valuestring) {
        strncpy(job.goal_name, goal_json->valuestring, sizeof(job.goal_name) - 1);
    }
    cJSON *utility_json = cJSON_GetObjectItem(root, "utility");
    if (cJSON_IsNumber(utility_json)) {
        float u = (float)utility_json->valuedouble;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
        job.utility = u;
    }
    cJSON_Delete(root); // Удаляем безопасно

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "execute_op", sizeof(resp->name) - 1);

    if (job.goal_name[0] == '\0') {
        const char *err = "{\"error\": \"missing required field 'goal'\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    int rc = db_write_sync(activate_goal_txn_fn, &job);
    if (rc != 0) {
        const char *err = "{\"error\": \"failed to activate goal in working memory\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
    } else {
        snprintf(resp->payload, sizeof(resp->payload),
                "{\"status\": \"queued_in_working_memory\", \"goal_id\": %llu}",
                (unsigned long long)job.goal_id);
    }
    resp->payload_size = (uint32_t)strlen(resp->payload);
}
