// ipc/handlers/command/cmd_execute.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "ipc/ipc.h"
#include "storage/db/db.h"
#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/register/register.h"
#include "runtime/operator/operator.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"

typedef struct {
    IPCPacket *req;
    cJSON *response_payload;
    int vm_status;
} ExecuteJob;

static int execute_op_txn_fn(MDB_txn *txn, void *arg) {
    ExecuteJob *job = (ExecuteJob *)arg;
    cJSON *root = cJSON_Parse((const char *)job->req->payload);
    if (!root) return -1;

    cJSON *op_str = cJSON_GetObjectItem(root, "op");
    if (!cJSON_IsString(op_str)) {
        cJSON_Delete(root);
        return -1;
    }

    // Поиск ID оператора по имени
    OperatorID op_id = operator_find_by_name(op_str->valuestring);
    const Operator *op = operator_find(op_id);
    if (!op) {
        cJSON_Delete(root);
        return -1;
    }

    // Инициализируем VMContext через vm_init из vm.c
    VMContext ctx;
    if (vm_init(&ctx, txn, NULL) != VM_OK) {
        cJSON_Delete(root);
        return -1;
    }

    // Инициализация HyperMemory
    ctx.hyper_mem = hyper_memory_new(txn,
                                     db.graph.hyper.atoms,
                                     db.graph.hyper.idx_process,
                                     db.graph.hyper.idx_args,
                                     db.graph.hyper.idx_context);

    // Формируем инструкцию
    Instruction ins = {0};
    ins.operator_id = op_id;

    cJSON *args = cJSON_GetObjectItem(root, "args");
    if (cJSON_IsArray(args)) {
        for (int i = 0; i < cJSON_GetArraySize(args) && i < 6; i++) {
            ins.arg[i] = (uint32_t)cJSON_GetNumberValue(cJSON_GetArrayItem(args, i));
        }
    }

    // Заполняем регистры
    cJSON *regs = cJSON_GetObjectItem(root, "regs");
    if (cJSON_IsObject(regs)) {
        cJSON *reg_val = regs->child;
        while (reg_val) {
            int reg_idx = atoi(reg_val->string);
            if (reg_idx >= 0 && reg_idx < VM_MAX_REGISTERS) {
                ctx.reg[reg_idx].type = REG_INT;
                if (cJSON_IsString(reg_val)) {
                    // 62-битные ID (djb2_hash) теряют точность при
                    // round-trip через JSON double (только 53 бита
                    // мантиссы) — хэшируем на сервере, как resolve_arg().
                    ctx.reg[reg_idx].i = (int64_t)djb2_hash(reg_val->valuestring);
                } else {
                    ctx.reg[reg_idx].i = (int64_t)reg_val->valuedouble;
                }
            }
            reg_val = reg_val->next;
        }
    }

    // Выполняем оператор
    job->vm_status = operator_execute(&ctx, op, &ins);

    // Упаковываем результат
    job->response_payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(job->response_payload, "status", (double)job->vm_status);

    cJSON *out_regs = cJSON_AddObjectToObject(job->response_payload, "regs");
    int sp_base = (int)ins.arg[2];
    uint32_t r_count    = ins.arg[3];
    uint32_t r_varcount = ins.arg[4];
    int64_t count_val    = (r_count    < VM_MAX_REGISTERS) ? ctx.reg[r_count].i    : 0;
    int64_t varcount_val = (r_varcount < VM_MAX_REGISTERS) ? ctx.reg[r_varcount].i : 0;

    cJSON *scratchpad_json = cJSON_AddArrayToObject(job->response_payload, "scratchpad");

    cJSON_AddNumberToObject(out_regs, "count", (double)count_val);
    cJSON_AddNumberToObject(out_regs, "var_count", (double)varcount_val);
    int count = (int)count_val;
    int var_count = (int)varcount_val;


    for (int i = 0; i < count * var_count && (sp_base + i) < MAX_SCRATCHPAD; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)ctx.scratchpad[sp_base + i].value);
        cJSON_AddItemToArray(scratchpad_json, cJSON_CreateString(buf));
    }

    // Опционально: значения конкретных регистров после выполнения.
    // В отличие от scratchpad-дампа выше (специфичен для OP_MATCH_PATTERN),
    // это ОБЩИЙ механизм чтения результата любого оператора — в т.ч.
    // OP_EXEC_ALGORITHM, где алгоритм кладёт результат в произвольный регистр.
    cJSON *report_regs = cJSON_GetObjectItem(root, "report_regs");
    if (cJSON_IsArray(report_regs)) {
        cJSON *reported = cJSON_AddObjectToObject(job->response_payload, "reported_regs");
        int n = cJSON_GetArraySize(report_regs);
        for (int i = 0; i < n; i++) {
            int reg_idx = (int)cJSON_GetNumberValue(cJSON_GetArrayItem(report_regs, i));
            if (reg_idx < 0 || reg_idx >= VM_MAX_REGISTERS) continue;
            char key[16];
            snprintf(key, sizeof(key), "%d", reg_idx);
            const Register *r = &ctx.reg[reg_idx];
            switch (r->type) {
                case REG_INT:   cJSON_AddNumberToObject(reported, key, (double)r->i); break;
                case REG_FLOAT: cJSON_AddNumberToObject(reported, key, r->f); break;
                case REG_BOOL:  cJSON_AddBoolToObject(reported, key, r->b); break;
                case REG_NODE:  cJSON_AddNumberToObject(reported, key, (double)r->node); break;
                default:        cJSON_AddNullToObject(reported, key); break;
            }
        }
    }

    // Очистка выделенной памяти и уничтожение VM
    if (ctx.hyper_mem) {
        hyper_memory_free(ctx.hyper_mem);
    }
    vm_destroy(&ctx);
    cJSON_Delete(root);
    return 0;
}

void cmd_execute_op(IPCPacket *req, IPCPacket *resp) {
    ExecuteJob job = { .req = req, .response_payload = NULL, .vm_status = -1 };

    // Выполняем под MDB_RDONLY транзакцией напрямую через LMDB
    MDB_txn *txn = NULL;
    int rc = mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn);
    if (rc == MDB_SUCCESS) {
        execute_op_txn_fn(txn, &job);
        mdb_txn_abort(txn); // Синхронный чистый abort для RO-транзакции
    }

    resp->type = IPC_RESPONSE;
    if (job.response_payload) {
        snprintf((char *)resp->name, sizeof(resp->name), "execute_op");
        const char *json_str = cJSON_PrintUnformatted(job.response_payload);
        snprintf((char *)resp->payload, sizeof(resp->payload), "%s", json_str);
        free((void *)json_str);
    } else {
        snprintf((char *)resp->name, sizeof(resp->name), "error");
        snprintf((char *)resp->payload, sizeof(resp->payload), "{\"error\": \"VM execution failed\"}");
    }
    resp->payload_size = (uint32_t)strlen((const char *)resp->payload);
    if (job.response_payload) {
        cJSON_Delete(job.response_payload);
    }
}
