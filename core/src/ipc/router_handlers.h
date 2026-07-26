// ipc/router_handlers.c
#ifndef IPC_HANDLERS_H
#define IPC_HANDLERS_H

#include "ipc/ipc.h"
#include "memory/working.h"

/* requests */
void req_ping(IPCPacket *req, IPCPacket *resp);
void req_generate_reply(IPCPacket *req, IPCPacket *resp);
void req_retrieve(IPCPacket *req, IPCPacket *resp);
void req_retrieve_graph(IPCPacket *req, IPCPacket *resp);
void req_execute_command(IPCPacket *req, IPCPacket *resp);
void req_get_command_result(IPCPacket *req, IPCPacket *resp);
void req_embedding(IPCPacket *req, IPCPacket *resp);
void req_rerank(IPCPacket *req, IPCPacket *resp);
void req_get_research_tasks(IPCPacket *req, IPCPacket *resp);

/* responses */
void resp_generate_reply(IPCPacket *req, IPCPacket *resp);
void resp_embedding(IPCPacket *req, IPCPacket *resp);
void resp_rerank(IPCPacket *req, IPCPacket *resp);

/* events */
void evt_generate_reply(IPCPacket *req, IPCPacket *resp);
void evt_embedding(IPCPacket *req, IPCPacket *resp);
void evt_rerank(IPCPacket *req, IPCPacket *resp);

/* commands */
void cmd_learn(IPCPacket *req, IPCPacket *resp);
void cmd_shutdown(IPCPacket *req, IPCPacket *resp);
void cmd_think(IPCPacket *req, IPCPacket *resp);

#endif // IPC_HANDLERS_H
