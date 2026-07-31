// ipc/ipc.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define IPC_NAME_SIZE     64
#define IPC_PAYLOAD_SIZE  65536

#define IPC_FLAG_JSON   0x00000000  // Добавляем бинарные флаги для поля packet->flags
#define IPC_FLAG_BINARY 0x00000001

typedef enum {
    IPC_REQUEST = 0,
    IPC_RESPONSE,
    IPC_COMMAND,
    IPC_EVENT
} IPCType;

typedef enum {
    IPC_OK = 0,
    IPC_ERROR = -1,
    IPC_DISCONNECTED = -2,
    IPC_TIMEOUT = -3,
    IPC_ERR_PAYLOAD_TOO_LARGE = -4
} IPCStatus;

typedef struct {
    uint64_t id;
    uint64_t parent_id;
    uint64_t timestamp;

    IPCType type;

    char source[32];
    char destination[32];
    char name[IPC_NAME_SIZE];

    uint32_t payload_size;

    // Меняем char на uint8_t для корректной работы с бинарными данными
    char payload[IPC_PAYLOAD_SIZE];

    uint32_t flags;
} IPCPacket;

typedef struct {
    int fd;
    pthread_t thread;
    volatile int alive;
} IPCClient;

/* ===== Lifecycle ===== */

IPCStatus ipc_init(void);
void ipc_shutdown(void);

/* ===== Messaging ===== */

IPCStatus ipc_send(const IPCPacket *packet);
IPCStatus ipc_receive(IPCPacket *packet);

/* ===== Routing ===== */

int ipc_dispatch(IPCPacket *req, IPCPacket *resp);

/* ===== Serialization ===== */

IPCStatus ipc_packet_to_json(const IPCPacket *packet, char *buffer, size_t size);

IPCStatus ipc_packet_from_json(const char *json, IPCPacket *packet);
