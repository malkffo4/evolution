// ipc/handlers/request/req.c
#include <string.h>
#include <stdlib.h>

#include "ipc/ipc.h"
#include <cjson/cJSON.h>
#include "runtime/logging/logging.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "knowledge/hyper_retrieval.h"
#include "math/hash.h"              // djb2_hash()
#include "execution/executor.h"     // executor_run_sync()
#include "types/exec.h"             // EXEC
#include "core/globals.h"

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

void req_retrieve(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse(req->payload);
    char query[256] = {0};
    if (json) {
        cJSON *query_item = cJSON_GetObjectItemCaseSensitive(json, "query");
        if (cJSON_IsString(query_item) && query_item->valuestring)
            strncpy(query, query_item->valuestring, sizeof(query) - 1);
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

    uint64_t participant_id = djb2_hash(query);
    LOG_IPC("Hyper-retrieve for '%s' (hash: %lu)", query, participant_id);

    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == MDB_SUCCESS) {
        HyperMemory local_hm;
        local_hm.txn = txn;
        local_hm.dbi_atoms = db.graph.hyper.atoms;
        local_hm.dbi_idx_process = db.graph.hyper.idx_process;
        local_hm.dbi_idx_args = db.graph.hyper.idx_args;
        local_hm.dbi_idx_context = db.graph.hyper.idx_context;

        char *result = hyper_retrieve_json(&local_hm, participant_id, 2, 30);
        mdb_txn_abort(txn);
        if (result) {
            strncpy(resp->payload, result, IPC_PAYLOAD_SIZE - 1);
            resp->payload_size = (uint32_t)strlen(result);
            free(result);
        } else {
            const char* err = "{\"error\": \"No results\"}";
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

void req_execute_command(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse(req->payload);
    const char *cmd_str = NULL;

    if (json) {
        cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(json, "command");
        if (cJSON_IsString(cmd_item) && cmd_item->valuestring && strlen(cmd_item->valuestring) > 0) {
            cmd_str = cmd_item->valuestring;
        } else {
            // Если команды нет или она пустая — удаляем JSON сразу
            cJSON_Delete(json);
            json = NULL;
        }
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "execute", sizeof(resp->name) - 1);

    if (!cmd_str) {
        const char *err = "{\"error\": \"Empty command provided\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        if (json) cJSON_Delete(json);
        return;
    }

    // Пока json существует, cmd_str валиден. Передаём его прямо в вызов.
    char *exec_args[] = { (char *)cmd_str, NULL };
    int task_id = 0;

    // Ставим задачу в асинхронную очередь экзекьютора. Поток не блокируется.
    int rc = executor_enqueue_script("/bin/sh", "-c", exec_args, &task_id);

    // Теперь JSON можно безопасно удалить
    cJSON_Delete(json);

    if (rc == 0) {
        // Мгновенно возвращаем Python-клиенту ID задачи для дальнейшего пуллинга
        snprintf(resp->payload, sizeof(resp->payload),
                 "{\"task_id\": %d, \"status\": \"queued\"}", task_id);
    } else {
        snprintf(resp->payload, sizeof(resp->payload),
                 "{\"error\": \"Failed to enqueue task\"}");
    }

    resp->payload_size = (uint32_t)strlen(resp->payload);
}

void req_get_command_result(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse(req->payload);
    int task_id = 0;

    if (json) {
        cJSON *id_item = cJSON_GetObjectItemCaseSensitive(json, "task_id");
        if (cJSON_IsNumber(id_item)) {
            task_id = id_item->valueint;
        }
        cJSON_Delete(json);
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "execute_result", sizeof(resp->name)-1);

    if (task_id <= 0) {
        const char* err = "{\"error\": \"Invalid task_id\"}";
        strncpy(resp->payload, err, sizeof(resp->payload)-1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    char *output = NULL;
    int exit_code = 0;
    int term_signal = 0;

    // Пытаемся забрать результат. Функция не блокирует поток.
    int rc = executor_get_result(task_id, &output, &exit_code, &term_signal);

    if (rc == 0) {
        // Результат готов и успешно извлечен
        cJSON *res_json = cJSON_CreateObject();
        cJSON_AddStringToObject(res_json, "status", "completed");
        cJSON_AddNumberToObject(res_json, "exit_code", exit_code);

        if (term_signal > 0) {
            cJSON_AddNumberToObject(res_json, "term_signal", term_signal);
        }

        cJSON_AddStringToObject(res_json, "output", output ? output : "");

        char *json_str = cJSON_PrintUnformatted(res_json);
        strncpy(resp->payload, json_str, IPC_PAYLOAD_SIZE - 1);
        resp->payload_size = (uint32_t)strlen(json_str);

        free(json_str);
        cJSON_Delete(res_json);

        // executor_get_result выделяет память под строку, мы обязаны ее освободить
        if (output) free(output);
    } else {
        // Задачи с таким ID нет в готовых (либо еще выполняется, либо ID неверный)
        const char* pending = "{\"status\": \"pending\"}";
        strncpy(resp->payload, pending, sizeof(resp->payload)-1);
        resp->payload_size = (uint32_t)strlen(pending);
    }
}
