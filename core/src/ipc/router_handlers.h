// ipc/ipc_handlers.h
#ifndef IPC_HANDLERS_H
#define IPC_HANDLERS_H

#include "ipc/ipc.h"

/* requests */

void req_ping(IPCPacket *p);
void req_generate_reply(IPCPacket *);
void req_embedding(IPCPacket *);
void req_rerank(IPCPacket *);

/* responses */

void resp_generate_reply(IPCPacket *);
void resp_embedding(IPCPacket *);
void resp_rerank(IPCPacket *);

/* events */

void evt_generate_reply(IPCPacket *);
void evt_embedding(IPCPacket *);
void evt_rerank(IPCPacket *);

/* commands */

void cmd_learn(IPCPacket *);
void cmd_shutdown(IPCPacket *);

#endif // IPC_HANDLERS_H
