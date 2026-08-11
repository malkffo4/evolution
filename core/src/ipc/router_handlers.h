// ipc/router_handlers.c
#pragma once

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
void req_get_score(IPCPacket *req, IPCPacket *resp);
void req_get_episodes(IPCPacket *req, IPCPacket *resp);
void req_audit_atoms(IPCPacket *req, IPCPacket *resp);
void req_get_property(IPCPacket *req, IPCPacket *resp);
void req_get_stats(IPCPacket *req, IPCPacket *resp);
void req_find_similar(IPCPacket *req, IPCPacket *resp);

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
void cmd_execute_op(IPCPacket *req, IPCPacket *resp);
void cmd_mark_flaw(IPCPacket *req, IPCPacket *resp);
void cmd_clear_cooldown(IPCPacket *req, IPCPacket *resp);
