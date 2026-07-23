// ipc/router.c
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ipc/ipc.h"
#include "ipc/router.h"
#include "ipc/router_handlers.h"

static const Route request_routes[] = {
    { "ping",           req_ping },
    { "generate_reply", req_generate_reply },
    { "chat",           req_generate_reply },
    { "retrieve",       req_retrieve },
    { "execute",        req_execute_command},
    { "embedding",      req_embedding },
    { "rerank",         req_rerank },
    { NULL, NULL }
};

static const Route response_routes[] = {
    { "generate_reply", resp_generate_reply },
    { "embedding",      resp_embedding },
    { "rerank",         resp_rerank },
    { NULL, NULL }
};

static const Route command_routes[] = {
    { "learn",          cmd_learn },
    { "shutdown",       cmd_shutdown },
    { NULL, NULL }
};

static const Route event_routes[] = {
    { "generate_reply", evt_generate_reply },
    { "embedding",      evt_embedding },
    { "rerank",         evt_rerank },
    { NULL, NULL }
};

static const Route *routes[] = {
    [IPC_REQUEST]  = request_routes,
    [IPC_RESPONSE] = response_routes,
    [IPC_COMMAND]  = command_routes,
    [IPC_EVENT]    = event_routes
};

static void ipc_send_error(IPCPacket *req, IPCPacket *resp, const char *message);
static int dispatch_table(const Route *table, IPCPacket *req, IPCPacket *resp);

static Handler find_handler(const Route *table, const char *name) {
    for (; table->name; ++table) {
        if (strcmp(table->name, name) == 0)
            return table->handler;
    }
    return NULL;
}

int ipc_dispatch(IPCPacket *req, IPCPacket *resp) {
    if (!req || !resp)
        return IPC_ERROR;

    if (req->type >= sizeof(routes)/sizeof(routes[0]))
        return IPC_ERROR;

    return dispatch_table(routes[req->type], req, resp);
}

static int dispatch_table(const Route *table, IPCPacket *req, IPCPacket *resp) {
    if (!table) {
        ipc_send_error(req, resp, "Invalid route table");
        return IPC_ERROR;
    }

    Handler h = find_handler(table, req->name);
    if (!h) {
        ipc_send_error(req, resp, "Unknown route");
        return IPC_ERROR;
    }

    h(req, resp);
    return IPC_OK;
}

void ipc_send_error(IPCPacket *req, IPCPacket *resp, const char *message) {
    (void)req;
    resp->type = IPC_RESPONSE;
    snprintf(resp->name, sizeof(resp->name), "error");
    snprintf(resp->payload, sizeof(resp->payload), "%s", message);
    resp->payload_size = (uint32_t)strlen(resp->payload);
}
