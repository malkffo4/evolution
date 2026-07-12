// ipc/ipc.h
#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define IPC_NAME_SIZE     64
#define IPC_PAYLOAD_SIZE  65536 // Увеличено с 4096 для передачи графов из RAG

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
    IPC_TIMEOUT = -3
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

int ipc_dispatch(IPCPacket *env);

/* ===== Serialization ===== */

IPCStatus ipc_packet_to_json(const IPCPacket *packet, char *buffer, size_t size);

IPCStatus ipc_packet_from_json(const char *json, IPCPacket *packet);

#endif // IPC_H
