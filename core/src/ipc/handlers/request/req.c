// ipc/handlers/request/req.c
#include <string.h>

#include "ipc/ipc.h"
#include "lib/cJSON.h"
#include "runtime/logging/logging.h"

void req_ping(IPCPacket *req, IPCPacket *resp) {
    LOG_IPC("Handling ping request id=%lu", req->id);
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "ping", sizeof(resp->name)-1);
    const char* ok_msg = "{\"ok\": true}";
    strncpy(resp->payload, ok_msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(ok_msg);
    LOG_IPC("Ping response prepared, payload: %s", resp->payload);
}

void req_generate_reply(IPCPacket *req, IPCPacket *resp) {
    // Парсим payload
    cJSON *json = cJSON_Parse(req->payload);
    char reply[IPC_PAYLOAD_SIZE] = {0};
    char extracted_text[1024] = {0};
    int has_text = 0;

    if (json) {
        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(json, "text");
        if (cJSON_IsString(text_item) && text_item->valuestring) {
            // Копируем строку в буфер на стеке ПЕРЕД удалением JSON-объекта
            strncpy(extracted_text, text_item->valuestring, sizeof(extracted_text) - 1);
            extracted_text[sizeof(extracted_text) - 1] = '\0';
            has_text = 1;
        }
        cJSON_Delete(json); // Теперь удаление безопасно
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "chat", sizeof(resp->name)-1);

    if (has_text) {
        // Безопасно форматируем ответ из локального буфера
        snprintf(reply, sizeof(reply), "Received: %s", extracted_text);
    } else {
        snprintf(reply, sizeof(reply), "Hello, I am Evolution Core!");
    }

    strncpy(resp->payload, reply, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(reply);
}

void req_embedding(IPCPacket *req, IPCPacket *resp) {
    (void)req;
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "error", sizeof(resp->name)-1);
    const char* msg = "Not implemented";
    strncpy(resp->payload, msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(msg);
}

void req_rerank(IPCPacket *req, IPCPacket *resp) {
    (void)req;
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "error", sizeof(resp->name)-1);
    const char* msg = "Not implemented";
    strncpy(resp->payload, msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(msg);
}
