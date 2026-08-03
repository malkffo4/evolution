// core/src/ipc/emitter.c
#include <string.h>
#include <stdio.h>
#include "ipc/ipc.h"

// Универсальная функция для отправки асинхронных событий из любой точки С-ядра.
// Она кладет пакет в tx_bus, откуда broadcast-поток раздаст его всем подписчикам.
void ipc_emit_event(const char *name, const char *payload_json) {
    IPCPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = IPC_EVENT;
    strncpy(pkt.name, name, sizeof(pkt.name) - 1);

    if (payload_json) {
        strncpy((char *)pkt.payload, payload_json, sizeof(pkt.payload) - 1);
        pkt.payload_size = (uint32_t)strlen((char *)pkt.payload);
    }

    // ipc_send безопасно забирает лок на tx_bus и мгновенно возвращает управление
    ipc_send(&pkt);
}
