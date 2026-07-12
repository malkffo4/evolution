#include <string.h>
#include <stdio.h>
#include <lmdb.h>

#include "ipc/router_handlers.h"
#include "memory/working.h"
#include "perception/perception.h"
#include "storage/db/db.h"

extern WorkingMemory global_wm; // Или прокинь через Context

void req_ping(IPCPacket *p)
{
    IPCPacket resp = {0};
    resp.id = p->id;
    resp.type = IPC_RESPONSE;
    strncpy(resp.name, "ping", sizeof(resp.name)-1);
    const char* ok_msg = "{\"ok\": true}";
    strncpy(resp.payload, ok_msg, sizeof(resp.payload)-1);
    resp.payload_size = strlen(ok_msg);
    ipc_send(&resp);
}

void req_generate_reply(IPCPacket *p)
{
    (void)p;
}

void req_embedding(IPCPacket *p)
{
    (void)p;
}

void req_rerank(IPCPacket *p)
{
    (void)p;
}

void resp_generate_reply(IPCPacket *p)
{
    (void)p;
}

void resp_embedding(IPCPacket *p)
{
    (void)p;
}

void resp_rerank(IPCPacket *p)
{
    (void)p;
}

void evt_generate_reply(IPCPacket *p)
{
    (void)p;
}

void evt_embedding(IPCPacket *p)
{
    (void)p;
}

void evt_rerank(IPCPacket *p)
{
    (void)p;
}

void cmd_learn(IPCPacket *p) {
    // Запускаем транзакцию записи
    MDB_txn *txn;
    mdb_txn_begin(db.env, NULL, 0, &txn);

    // Передаем JSON из Python в графовый процессор
    if (perceive_and_activate(p->payload, &global_wm, txn) == 0) {
        mdb_txn_commit(txn);
        printf("\033[32m[IPC KNOWLEDGE]\033[0m Усвоено новых знаний: %d байт\n", p->payload_size);
    } else {
        mdb_txn_abort(txn);
        printf("\033[31m[IPC ERROR]\033[0m Ошибка парсинга знаний!\n");
    }
}
void cmd_shutdown(IPCPacket *p)
{
    (void)p;
}
