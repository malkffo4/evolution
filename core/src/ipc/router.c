// ipc/router.c
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ipc/ipc.h"
#include "ipc/router.h"
#include "ipc/router_handlers.h"


static const Route request_routes[] =
{
    { "ping",           req_ping },
    { "generate_reply", req_generate_reply },
    { "embedding",      req_embedding },
    { "rerank",         req_rerank },
    { NULL, NULL }
};

static const Route response_routes[] =
{
    { "generate_reply", resp_generate_reply },
    { "embedding",      resp_embedding },
    { "rerank",         resp_rerank },
    { NULL, NULL }
};

static const Route command_routes[] =
{
    { "learn",          cmd_learn },
    { "shutdown",       cmd_shutdown },
    { NULL, NULL }
};

static const Route event_routes[] =
{
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

static void ipc_send_error(IPCPacket *, const char *);
static int dispatch_table(const Route *, IPCPacket *);

static Handler find_handler(const Route *table, const char *name) {
    for (; table->name; ++table) {
        if (strcmp(table->name, name) == 0)
            return table->handler;
    }

    return NULL;
}

int ipc_dispatch(IPCPacket *p) {
    if (!p)
        return IPC_ERROR;

    if (p->type >= sizeof(routes)/sizeof(routes[0]))
        return IPC_ERROR;

    return dispatch_table(routes[p->type], p);
}

static int dispatch_table(const Route *table, IPCPacket *p) {
    if (!table)
        return IPC_ERROR;

    Handler h = find_handler(table, p->name);

    if (!h) {
        ipc_send_error(p, "Unknown route");
        return IPC_ERROR;
    }

    h(p);

    return IPC_OK;
}

void ipc_send_error(IPCPacket *request, const char *message) {
    IPCPacket response = {0};

    response.id = request->id;
    response.type = IPC_RESPONSE;

    snprintf(response.name, sizeof(response.name), "error");
    snprintf(response.payload, sizeof(response.payload), "%s", message);
    response.payload_size = (uint32_t)strlen(response.payload);

    ipc_send(&response);
}
