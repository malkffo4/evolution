// ipc/router.c
#pragma once

#include "ipc/transport.h"

typedef void (*Handler)(IPCPacket *req, IPCPacket *resp);

typedef struct {
    const char *name;
    Handler handler;
} Route;

typedef struct {
    uint64_t request_id;
    IPCClient *client;
} RequestRoute;
