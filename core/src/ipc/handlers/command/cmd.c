// ipc/handlers/command/cmd.c
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <lmdb.h>

#include "core/globals.h"
#include "core/message_bus.h"
#include "ipc/router_handlers.h"
#include "ipc/ipc.h"
#include <cjson/cJSON.h>
#include "knowledge/algorithm_saver.h"
#include "knowledge/pipeline_io.h"
#include "perception/perception.h"
#include "memory/working.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/logging/logging.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"

// TODO. Need remove JSON ??? For example move them to perception
void cmd_learn(IPCPacket *req, IPCPacket *resp) {
    MDB_txn *txn;
    int rc;
    if (mdb_txn_begin(db.env, NULL, 0, &txn) == MDB_SUCCESS) {
        cJSON *root = cJSON_Parse(req->payload);
        if (root) {
            cJSON *type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "pipeline") == 0) {
                cJSON_Delete(root);
                rc = pipeline_import_json(req->payload, txn);
                if (rc == 0) {
                    mdb_txn_commit(txn);
                    resp->type = IPC_RESPONSE;
                    snprintf(resp->name, sizeof(resp->name), "learn");
                    snprintf(resp->payload, sizeof(resp->payload), "{\"ok\": true, \"msg\": \"pipeline saved\"}");
                    resp->payload_size = (uint32_t)strlen(resp->payload);
                    return;
                } else {
                    mdb_txn_abort(txn);
                    goto error;
                }
            }
            cJSON_Delete(root);
        }

        // не pipeline – гипер‑атомы
        if (perceive_hyper_json(req->payload, txn, global_hyper_mem) == 0) {
            mdb_txn_commit(txn);
            resp->type = IPC_RESPONSE;
            snprintf(resp->name, sizeof(resp->name), "learn");
            snprintf(resp->payload, sizeof(resp->payload), "{\"ok\": true}");
            resp->payload_size = (uint32_t)strlen(resp->payload);
            return;
        }
        if (perceive_and_activate(req->payload, &global_wm, txn, global_hyper_mem) == 0) {
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
