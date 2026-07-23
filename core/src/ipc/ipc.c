// ipc/ipc.c
#include "ipc.h"

#include "core/message_bus.h"
#include "ipc/transport.h"

IPCStatus ipc_init(void) {
    if (bus_init() != IPC_OK)
        return IPC_ERROR;

    if (transport_server_start() != IPC_OK)
        return IPC_ERROR;

    return IPC_OK;
}

void ipc_shutdown(void) {
    transport_server_stop();
    // bus_shutdown();
    bus_stop();
}

IPCStatus ipc_send(const IPCPacket *packet) {
    return bus_tx_publish(packet);
}

IPCStatus ipc_receive(IPCPacket *packet) {
    return bus_rx_pop(packet);
}
