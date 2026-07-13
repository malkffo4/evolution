// ipc/handlers/command/cmd.c
#include <string.h>
#include <lmdb.h>
#include "main.h"                // g_running
#include "ipc/router_handlers.h" // global_wm
#include "storage/db/db.h"
#include "ipc/ipc.h"
#include "lib/cJSON.h"
#include "perception/perception.h"
#include "core/message_bus.h"
#include "memory/working.h"
#include "runtime/logging/logging.h"

void cmd_learn(IPCPacket *req, IPCPacket *resp) {
    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, 0, &txn) == MDB_SUCCESS) {
        if (perceive_and_activate(req->payload, &global_wm, txn) == 0) {
            mdb_txn_commit(txn);
            // Успешный ответ
            resp->type = IPC_RESPONSE;
            strncpy(resp->name, "learn", sizeof(resp->name)-1);
            const char* ok_msg = "{\"ok\": true}";
            strncpy(resp->payload, ok_msg, sizeof(resp->payload)-1);
            resp->payload_size = (uint32_t)strlen(ok_msg);
            return;
        } else {
            mdb_txn_abort(txn);
        }
    }
    // Ошибка
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "error", sizeof(resp->name)-1);
    const char* msg = "Learn failed";
    strncpy(resp->payload, msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(msg);
}

void cmd_shutdown(IPCPacket *req, IPCPacket *resp) {
    LOG_INFO("Get command for Shutting down...");
    (void)req;

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "shutdown", sizeof(resp->name)-1);
    const char* ok_msg = "{\"ok\": true}";
    strncpy(resp->payload, ok_msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(ok_msg);

    // УСТРАНЕНИЕ БАГА: Выставляем флаг остановки и будим спящие шины.
    // Больше никакой деструктивной логики и pthread_join() внутри самого сетевого потока!
    g_running = 0;
    bus_stop();
}
