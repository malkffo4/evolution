#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc/ipc.h"
#include "lib/cJSON.h"

// TODO
//1. replace strncpy by snprintf(packet->source, sizeof(packet->source), "%s", item->valuestring);

IPCStatus ipc_packet_to_json(const IPCPacket *packet, char *buffer, size_t size) {
    if (!packet || !buffer)
        return IPC_ERROR;

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return IPC_ERROR;

    cJSON_AddNumberToObject(root, "id", (double)packet->id);
    cJSON_AddNumberToObject(root, "parent_id", (double)packet->parent_id);
    cJSON_AddNumberToObject(root, "timestamp", (double)packet->timestamp);

    cJSON_AddNumberToObject(root, "type", packet->type);

    cJSON_AddStringToObject(root, "source", packet->source);
    cJSON_AddStringToObject(root, "destination", packet->destination);
    cJSON_AddStringToObject(root, "name", packet->name);

    cJSON_AddStringToObject(root, "payload", packet->payload);
    cJSON_AddNumberToObject(root, "payload_size", packet->payload_size);

    char *json = cJSON_PrintUnformatted(root);

    if (!json) {
        cJSON_Delete(root);
        return IPC_ERROR;
    }

    strncpy(buffer, json, size - 1);
    buffer[size - 1] = '\0';

    free(json);
    cJSON_Delete(root);

    return IPC_OK;
}

IPCStatus ipc_packet_from_json(const char *json, IPCPacket *packet) {
    if (!json || !packet)
        return IPC_ERROR;

    cJSON *root = cJSON_Parse(json);
    if (!root)
        return IPC_ERROR;

    memset(packet, 0, sizeof(IPCPacket));

    // Макрос для безопасного копирования строки с проверкой обрезки
    #define SAFE_STRCPY(dest, src, dest_size, err_action) do { \
        int _ret = snprintf(dest, dest_size, "%s", src); \
        if (_ret < 0 || (size_t)_ret >= dest_size) { \
            cJSON_Delete(root); \
            err_action; \
        } \
    } while(0)

    cJSON *item;

    item = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsNumber(item))
        packet->id = (uint64_t)item->valuedouble;

    item = cJSON_GetObjectItem(root, "parent_id");
    if (cJSON_IsNumber(item))
        packet->parent_id = (uint64_t)item->valuedouble;

    item = cJSON_GetObjectItem(root, "timestamp");
    if (cJSON_IsNumber(item))
        packet->timestamp = (uint64_t)item->valuedouble;

    item = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsNumber(item))
        packet->type = item->valueint;

    item = cJSON_GetObjectItem(root, "source");
    if (cJSON_IsString(item))
        SAFE_STRCPY(packet->source, item->valuestring, sizeof(packet->source), return IPC_ERR_PAYLOAD_TOO_LARGE);

    item = cJSON_GetObjectItem(root, "destination");
    if (cJSON_IsString(item))
        SAFE_STRCPY(packet->destination, item->valuestring, sizeof(packet->destination), return IPC_ERR_PAYLOAD_TOO_LARGE);

    item = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(item))
        SAFE_STRCPY(packet->name, item->valuestring, IPC_NAME_SIZE, return IPC_ERR_PAYLOAD_TOO_LARGE);

    item = cJSON_GetObjectItem(root, "payload");
    if (cJSON_IsString(item)) {
        // payload нельзя обрезать – это должен быть целый JSON
        SAFE_STRCPY(packet->payload, item->valuestring, IPC_PAYLOAD_SIZE, return IPC_ERR_PAYLOAD_TOO_LARGE);

        item = cJSON_GetObjectItem(root, "payload_size");
        if (cJSON_IsNumber(item)) {
            packet->payload_size = (uint32_t)item->valueint;
        } else {
            packet->payload_size = (uint32_t)strlen(packet->payload);
        }
    }

    #undef SAFE_STRCPY

    cJSON_Delete(root);
    return IPC_OK;
}
