#include <string.h>
#include <stdio.h>
#include <lmdb.h>

#include "ipc/router_handlers.h"
#include "memory/working.h"
#include "perception/perception.h"
#include "storage/db/db.h"
#include "runtime/logging/logging.h"

extern WorkingMemory global_wm;

void req_ping(IPCPacket *req, IPCPacket *resp) {
    LOG_IPC("Handling ping request id=%lu", req->id);
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "ping", sizeof(resp->name)-1);
    const char* ok_msg = "{\"ok\": true}";
    strncpy(resp->payload, ok_msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(ok_msg);
    LOG_IPC("Ping response prepared, payload: %s", resp->payload);
}

void req_generate_reply(IPCPacket *req, IPCPacket *resp) {
    (void)req;
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "error", sizeof(resp->name)-1);
    const char* msg = "Not implemented";
    strncpy(resp->payload, msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(msg);
}

void req_embedding(IPCPacket *req, IPCPacket *resp) {
    (void)req;
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "error", sizeof(resp->name)-1);
    const char* msg = "Not implemented";
    strncpy(resp->payload, msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(msg);
}

void req_rerank(IPCPacket *req, IPCPacket *resp) {
    (void)req;
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "error", sizeof(resp->name)-1);
    const char* msg = "Not implemented";
    strncpy(resp->payload, msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(msg);
}

void resp_generate_reply(IPCPacket *req, IPCPacket *resp) {
    (void)req; (void)resp;
}

void resp_embedding(IPCPacket *req, IPCPacket *resp) {
    (void)req; (void)resp;
}

void resp_rerank(IPCPacket *req, IPCPacket *resp) {
    (void)req; (void)resp;
}

void evt_generate_reply(IPCPacket *req, IPCPacket *resp) {
    (void)req; (void)resp;
}

void evt_embedding(IPCPacket *req, IPCPacket *resp) {
    (void)req; (void)resp;
}

void evt_rerank(IPCPacket *req, IPCPacket *resp) {
    (void)req; (void)resp;
}

void cmd_learn(IPCPacket *req, IPCPacket *resp) {
    // Здесь обработка learn
    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, 0, &txn) == MDB_SUCCESS) {
        if (perceive_and_activate(req->payload, &global_wm, txn) == 0) {
            mdb_txn_commit(txn);
            // успех
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
    // ошибка
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "error", sizeof(resp->name)-1);
    const char* msg = "Learn failed";
    strncpy(resp->payload, msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(msg);
}

void cmd_shutdown(IPCPacket *req, IPCPacket *resp) {
    (void)req;
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "shutdown", sizeof(resp->name)-1);
    const char* ok_msg = "{\"ok\": true}";
    strncpy(resp->payload, ok_msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(ok_msg);
    // Тут можно вызвать exit, но лучше дать main.c обработать
}
