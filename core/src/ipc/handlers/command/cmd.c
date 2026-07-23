// ipc/handlers/command/cmd.c
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <lmdb.h>

#include "main.h"                // g_running
#include "core/globals.h"
#include "core/message_bus.h"
#include "ipc/router_handlers.h" // global_wm
#include "ipc/ipc.h"
#include "lib/cJSON.h"
#include "knowledge/algorithm_saver.h"
#include "perception/perception.h"
#include "memory/working.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/logging/logging.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"

// Старая логика:
// if (perceive_and_activate(req->payload, &global_wm, txn) == 0) ...

void cmd_learn(IPCPacket *req, IPCPacket *resp) {
    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, 0, &txn) == MDB_SUCCESS) {
        cJSON *root = cJSON_Parse(req->payload);
        if (root) {
            cJSON *type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "pipeline") == 0) {
                // Сохраняем Pipeline в таблицу algorithms
                uint64_t algo_id = (uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "algo_id"));
                cJSON *code_arr = cJSON_GetObjectItem(root, "code");
                if (cJSON_IsArray(code_arr)) {
                    uint32_t len = cJSON_GetArraySize(code_arr);
                    Instruction *code = malloc((size_t)len * sizeof(Instruction));
                    for (uint32_t i = 0; i < len; i++) {
                        cJSON *ins = cJSON_GetArrayItem(code_arr, i);
                        const char *op_name = cJSON_GetStringValue(cJSON_GetObjectItem(ins, "operator_id"));
                        // Преобразование имени в Opcode (можно сделать мапу, пока хардкод)
                        if (strcmp(op_name, "OP_CHECK_CACHED_EDGE") == 0) code[i].operator_id = 42; // OP_CHECK_CACHED_EDGE
                        else if (strcmp(op_name, "OP_HALT") == 0) code[i].operator_id = 1; // OP_HALT
                        else code[i].operator_id = 0;
                        cJSON *arg_arr = cJSON_GetObjectItem(ins, "arg");
                        for (int a = 0; a < 6; a++)
                            code[i].arg[a] = (uint32_t)cJSON_GetNumberValue(cJSON_GetArrayItem(arg_arr, a));
                    }
                    Pipeline p = { .code = code, .code_len = len, .capacity = len,
                                   .constants = {0} };
                    if (algorithm_save(txn, algo_id, &p) == MDB_SUCCESS) {
                        mdb_txn_commit(txn);
                        resp->type = IPC_RESPONSE;
                        snprintf(resp->name, sizeof(resp->name), "learn");
                        snprintf(resp->payload, sizeof(resp->payload), "{\"ok\": true, \"msg\": \"pipeline saved\"}");
                        resp->payload_size = (uint32_t)strlen(resp->payload);
                    } else {
                        mdb_txn_abort(txn);
                        goto error;
                    }
                    free(code);
                }
                cJSON_Delete(root);
                return;
            }
            cJSON_Delete(root);
        }

        // Если не pipeline, пробуем старый метод (perceive_hyper_json)
        if (perceive_hyper_json(req->payload, txn, global_hyper_mem) == 0) {
            mdb_txn_commit(txn);
            resp->type = IPC_RESPONSE;
            snprintf(resp->name, sizeof(resp->name), "learn");
            snprintf(resp->payload, sizeof(resp->payload), "{\"ok\": true}");
            resp->payload_size = (uint32_t)strlen(resp->payload);
            return;
        } else {
            mdb_txn_abort(txn);
        }
    }
error:
    resp->type = IPC_RESPONSE;
    snprintf(resp->name, sizeof(resp->name), "error");
    snprintf(resp->payload, sizeof(resp->payload), "Learn failed");
    resp->payload_size = (uint32_t)strlen(resp->payload);
}

void cmd_shutdown(IPCPacket *req, IPCPacket *resp) {
    LOG_INFO("Get command for Shutting down...");
    (void)req;

    resp->type = IPC_RESPONSE;
    snprintf(resp->name, sizeof(resp->name), "shutdown");
    snprintf(resp->payload, sizeof(resp->payload), "{\"ok\": true}");
    resp->payload_size = (uint32_t)strlen(resp->payload);

    // УСТРАНЕНИЕ БАГА: Выставляем флаг остановки и будим спящие шины.
    // Больше никакой деструктивной логики и pthread_join() внутри самого сетевого потока!
    g_running = 0;
    bus_stop();
}

void cmd_think(IPCPacket *req, IPCPacket *resp) {
    (void)req;

    // Принудительно дёргаем MainLoop вручную (без транзакции LMDB — демон сам управляет)
    // Для этого просто отправляем событие пробуждения демону,
    // либо, если у вас есть функция ручного запуска MainLoop, вызываем её.

    // Простейший вариант: ставим флаг, который демон проверяет.
    // Пока что просто отвечаем OK, что триггернуло демон.
    extern int g_think_trigger;  // объявим в subconscious.c
    g_think_trigger = 1;

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "think", sizeof(resp->name)-1);
    const char* ok_msg = "{\"ok\": true, \"msg\": \"MainLoop triggered\"}";
    strncpy(resp->payload, ok_msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(ok_msg);
}
