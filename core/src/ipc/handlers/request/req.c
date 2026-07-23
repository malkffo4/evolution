// ipc/handlers/request/req.c
#include <string.h>
#include <stdlib.h>
#include "ipc/ipc.h"
#include "lib/cJSON.h"
#include "runtime/logging/logging.h"
#include "storage/db/db.h"
#include "knowledge/retrieval.h"    // retrieve_subgraph_json()
#include "math/hash.h"              // djb2_hash()
#include "execution/executor.h"     // executor_run_sync()
#include "types/exec.h"             // EXEC

void req_ping(IPCPacket *req, IPCPacket *resp) {
    LOG_IPC("Handling ping request id=%lu", req->id);
    resp->type = IPC_RESPONSE;
    snprintf(resp->name, sizeof(resp->name), "ping");
    snprintf(resp->payload, sizeof(resp->payload), "{\"ok\": true}");
    resp->payload_size = (uint32_t)strlen(resp->payload);

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

// ЭНДПОИНТ: RAG Context Retrieval
void req_retrieve(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse(req->payload);
    char query[256] = {0};

    if (json) {
        cJSON *query_item = cJSON_GetObjectItemCaseSensitive(json, "query");
        if (cJSON_IsString(query_item) && query_item->valuestring) {
            strncpy(query, query_item->valuestring, sizeof(query) - 1);
        }
        cJSON_Delete(json);
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "retrieve", sizeof(resp->name)-1);

    if (strlen(query) == 0) {
        const char* err = "{\"error\": \"Empty query\"}";
        strncpy(resp->payload, err, sizeof(resp->payload)-1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    // Хешируем слово, чтобы найти его ID
    uint64_t node_id = djb2_hash(query);
    LOG_IPC("Retrieving context for query '%s' (hash: %lu)", query, node_id);

    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == MDB_SUCCESS) {
        // Извлекаем подграф (глубина 2 шага, до 30 узлов)
        char *graph_json = retrieve_subgraph_json(txn, node_id, 2, 30);
        mdb_txn_abort(txn); // Read-only транзакция просто закрывается

        if (graph_json) {
            strncpy(resp->payload, graph_json, IPC_PAYLOAD_SIZE - 1);
            resp->payload_size = (uint32_t)strlen(graph_json);
            free(graph_json);
        } else {
            const char* err = "{\"error\": \"Node not found or isolated\"}";
            strncpy(resp->payload, err, sizeof(resp->payload)-1);
            resp->payload_size = (uint32_t)strlen(err);
        }
    } else {
        const char* err = "{\"error\": \"DB transaction failed\"}";
        strncpy(resp->payload, err, sizeof(resp->payload)-1);
        resp->payload_size = (uint32_t)strlen(err);
    }
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

// Эндпоинт: Позволяет Python отправлять команды в консоль через C-ядро
void req_execute_command(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse(req->payload);
    char cmd[MAX_CMD_LENGTH] = {0};

    if (json) {
        cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(json, "command");
        if (cJSON_IsString(cmd_item) && cmd_item->valuestring) {
            strncpy(cmd, cmd_item->valuestring, sizeof(cmd) - 1);
        }
        cJSON_Delete(json);
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "execute", sizeof(resp->name)-1);

    if (strlen(cmd) == 0) {
        const char* err = "{\"error\": \"Empty command provided\"}";
        strncpy(resp->payload, err, sizeof(resp->payload)-1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    // Выполняем команду
    char output[MAX_OUTPUT_LENGTH] = {0};
    int exit_code = -1;

    ExecStatus status = executor_run_sync(cmd, output, sizeof(output), &exit_code);

    if (status == EXEC_OK) {
        cJSON *res_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(res_json, "exit_code", exit_code);
        cJSON_AddStringToObject(res_json, "output", output);

        char *json_str = cJSON_PrintUnformatted(res_json);
        strncpy(resp->payload, json_str, IPC_PAYLOAD_SIZE - 1);
        resp->payload_size = (uint32_t)strlen(json_str);

        free(json_str);
        cJSON_Delete(res_json);
    } else {
        const char* err = "{\"error\": \"Failed to execute command\"}";
        strncpy(resp->payload, err, sizeof(resp->payload)-1);
        resp->payload_size = (uint32_t)strlen(err);
    }
}
