#ifndef IPC_TRANSPORT_H
#define IPC_TRANSPORT_H

#include "ipc/ipc.h"

IPCStatus transport_server_start(void);
void transport_server_stop(void);

IPCStatus transport_send_fd(int fd, const IPCPacket *packet);

IPCStatus transport_recv_fd(int fd, IPCPacket *packet);

#endif
