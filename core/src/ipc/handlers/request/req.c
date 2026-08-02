// ipc/handlers/request/req.c
#include <string.h>
#include <stdlib.h>
#include <cjson/cJSON.h>

#include "ipc/ipc.h"
#include "runtime/logging/logging.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "knowledge/episode.h"
#include "knowledge/hyper_retrieval.h"
#include "knowledge/evaluation.h"
#include "math/hash.h"              // djb2_hash()
#include "execution/executor.h"     // executor_run_sync()
#include "types/exec.h"             // EXEC
#include "core/globals.h"
#include "memory/subconscious.h"
#include "storage/property/property.h"

#define AUDIT_BATCH_SIZE 500

/*
 * req_audit_atoms: постраничный курсорный обход db.graph.hyper.atoms.
 * Запрос:  {"resume_id": <ko_id_t, 0=начало>, "sti_threshold":f, "lti_threshold":f}
 * Ответ:   {"atoms":[...], "next_resume_id":id, "has_more":bool, "scanned":n}
 * Read-only, MDB_RDONLY открывается/закрывается прямо в IPC-потоке —
 * тот же паттерн, что req_get_episodes. Пагинация целиком на стороне
 * клиента: сервер не хранит состояние между вызовами (в отличие от
 * decay.c::g_resume_key, здесь клиент явно передаёт resume_id).
 */
void req_audit_atoms(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse((const char *)req->payload);
    ko_id_t resume_id = 0;
    float sti_threshold = 0.10f, lti_threshold = 0.10f;

    if (json) {
        cJSON *r = cJSON_GetObjectItem(json, "resume_id");
        cJSON *st = cJSON_GetObjectItem(json, "sti_threshold");
        cJSON *lt = cJSON_GetObjectItem(json, "lti_threshold");
        if (cJSON_IsNumber(r))  resume_id = (ko_id_t)r->valuedouble;
        if (cJSON_IsNumber(st)) sti_threshold = (float)st->valuedouble;
        if (cJSON_IsNumber(lt)) lti_threshold = (float)lt->valuedouble;
        cJSON_Delete(json);
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "audit_atoms", sizeof(resp->name) - 1);

    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) != MDB_SUCCESS) {
        const char *err = "{\"error\": \"DB transaction failed\"}";
        strncpy((char *)resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    MDB_cursor *cursor;
    if (mdb_cursor_open(txn, db.graph.hyper.atoms, &cursor) != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        const char *err = "{\"error\": \"cursor_open failed\"}";
        strncpy((char *)resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    MDB_val key, data;
    int rc = resume_id
        ? (key.mv_size = sizeof(ko_id_t), key.mv_data = &resume_id,
           mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE))
        : mdb_cursor_get(cursor, &key, &data, MDB_FIRST);

    cJSON *arr = cJSON_CreateArray();
    uint32_t scanned = 0;
    ko_id_t next_resume = 0;
    bool has_more = false;

    while (rc == MDB_SUCCESS && scanned < AUDIT_BATCH_SIZE) {
        if (data.mv_size == sizeof(NeuroAtom)) {
            NeuroAtom atom;
            memcpy(&atom, data.mv_data, sizeof(NeuroAtom));
            scanned++;

            bool is_cold = atom.sti < sti_threshold && atom.lti < lti_threshold;
            bool has_ref_args = false;
            for (int s = 0; s < HYPER_VAL_SLOTS; s++)
                if (HYPER_GET_TYPE(atom.args[s].raw) == HYPER_TYPE_REF && atom.args[s].raw != 0)
                    has_ref_args = true;

            if (is_cold || !has_ref_args) {
                cJSON *a = cJSON_CreateObject();
                cJSON_AddNumberToObject(a, "id", (double)atom.id);
                cJSON_AddNumberToObject(a, "process_id", (double)atom.process_id);
                cJSON_AddNumberToObject(a, "sti", atom.sti);
                cJSON_AddNumberToObject(a, "lti", atom.lti);
                cJSON_AddNumberToObject(a, "truth_confidence", atom.truth_confidence);
                cJSON_AddBoolToObject(a, "has_ref_args", has_ref_args);
                cJSON_AddItemToArray(arr, a);
            }
        }
        rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
    }

    if (rc == MDB_SUCCESS) {
        memcpy(&next_resume, key.mv_data, sizeof(ko_id_t));
        has_more = true;
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "atoms", arr);
    cJSON_AddNumberToObject(root, "next_resume_id", (double)next_resume);
    cJSON_AddBoolToObject(root, "has_more", has_more);
    cJSON_AddNumberToObject(root, "scanned", scanned);

    char *s = cJSON_PrintUnformatted(root);
    snprintf((char *)resp->payload, sizeof(resp->payload), "%s", s);
    resp->payload_size = (uint32_t)strlen((char *)resp->payload);
    free(s);
    cJSON_Delete(root);
}

void req_ping(IPCPacket *req, IPCPacket *resp) {
    LOG_IPC("Handling ping request id=%lu", req->id);
    resp->type = IPC_RESPONSE;
    snprintf(resp->name, sizeof(resp->name), "ping");
    snprintf((char *)resp->payload, sizeof(resp->payload), "{\"ok\": true}");
    resp->payload_size = (uint32_t)strlen((char *)resp->payload);

    LOG_IPC("Ping response prepared, payload: %s", resp->payload);
}

void req_generate_reply(IPCPacket *req, IPCPacket *resp) {
    // Парсим payload
    cJSON *json = cJSON_Parse((const char *)req->payload);
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

    strncpy((char *)resp->payload, reply, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(reply);
}

void req_retrieve(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse((const char *)req->payload);
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
        strncpy((char *)resp->payload, err, sizeof(resp->payload)-1);
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
            strncpy((char *)resp->payload, result, IPC_PAYLOAD_SIZE - 1);
            resp->payload[IPC_PAYLOAD_SIZE - 1] = '\0';

            // ФИКС: Берем длину уже обрезанного payload, а не потенциально гигантского result
            resp->payload_size = (uint32_t)strlen((char *)resp->payload);
            free(result);
        } else {
            const char* err = "{\"error\": \"No results\"}";
            strncpy((char *)resp->payload, err, sizeof(resp->payload)-1);
            resp->payload_size = (uint32_t)strlen(err);
        }
    } else {
        const char* err = "{\"error\": \"DB transaction failed\"}";
        strncpy((char *)resp->payload, err, sizeof(resp->payload)-1);
        resp->payload_size = (uint32_t)strlen(err);
    }
}

void req_retrieve_graph(IPCPacket *req, IPCPacket *resp) {
    (void)req;
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "error", sizeof(resp->name)-1);
    const char* msg = "Not implemented yet";
    strncpy((char *)resp->payload, msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(msg);
    // resp->type = IPC_RESPONSE;
    // strncpy(resp->name, "retrieve_graph", sizeof(resp->name)-1);

    // // Допустим, мы достали из LMDB массив из 10 000 Node ID
    // uint64_t node_ids[10000];
    // uint32_t count = get_nodes_from_db(node_ids, 10000);

    // // Прямое копирование байтов в payload без JSON!
    // size_t data_size = count * sizeof(uint64_t);
    // memcpy(resp->payload, node_ids, data_size);

    // resp->payload_size = (uint32_t)data_size;
    // resp->flags |= IPC_FLAG_BINARY; // Говорим Python-клиенту, что это сырые байты
}

void req_embedding(IPCPacket *req, IPCPacket *resp) {
    (void)req;
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "error", sizeof(resp->name)-1);
    const char* msg = "Not implemented";
    strncpy((char *)resp->payload, msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(msg);
}

void req_rerank(IPCPacket *req, IPCPacket *resp) {
    (void)req;
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "error", sizeof(resp->name)-1);
    const char* msg = "Not implemented";
    strncpy((char *)resp->payload, msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(msg);
}

void req_execute_command(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse((const char *)req->payload);
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
        strncpy((char *)resp->payload, err, sizeof(resp->payload) - 1);
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
        snprintf((char *)resp->payload, sizeof(resp->payload), "{\"task_id\": %d, \"status\": \"queued\"}", task_id);
    } else {
        snprintf((char *)resp->payload, sizeof(resp->payload), "{\"error\": \"Failed to enqueue task\"}");
    }

    resp->payload_size = (uint32_t)strlen((char *)resp->payload);
}

void req_get_command_result(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse((const char *)req->payload);
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
        strncpy((char *)resp->payload, err, sizeof(resp->payload)-1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    char *output = NULL;
    int exit_code = 0;
    int term_signal = 0;

    int rc = executor_get_result(task_id, &output, &exit_code, &term_signal);
    if (rc == 0) {
        cJSON *res_json = cJSON_CreateObject();
        cJSON_AddStringToObject(res_json, "status", "completed");
        cJSON_AddNumberToObject(res_json, "exit_code", exit_code);
        if (term_signal > 0) {
            cJSON_AddNumberToObject(res_json, "term_signal", term_signal);
        }
        cJSON_AddStringToObject(res_json, "output", output ? output : "");
        char *json_str = cJSON_PrintUnformatted(res_json);

        strncpy((char *)resp->payload, json_str, IPC_PAYLOAD_SIZE - 1);
        resp->payload[IPC_PAYLOAD_SIZE - 1] = '\0'; // Гарантируем нуль-терминатор

        // ФИКС: Берем длину уже обрезанного payload
        resp->payload_size = (uint32_t)strlen((char *)resp->payload);

        free(json_str);
        cJSON_Delete(res_json);
        if (output) free(output);
    } else {
        const char* pending = "{\"status\": \"pending\"}";
        strncpy((char *)resp->payload, pending, sizeof(resp->payload)-1);
        resp->payload_size = (uint32_t)strlen(pending);
    }
}

void req_get_research_tasks(IPCPacket *req, IPCPacket *resp) {
    (void)req;
    ResearchTask tasks[MAX_PENDING_TASKS];
    int count = get_pending_tasks(tasks, MAX_PENDING_TASKS);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "node_id", (double)tasks[i].node_id);
        cJSON_AddStringToObject(t, "query", tasks[i].query);
        cJSON_AddItemToArray(arr, t);
    }
    char *json_str = cJSON_PrintUnformatted(arr);
    snprintf((char *)resp->payload, sizeof(resp->payload), "%s", json_str);
    resp->payload_size = (uint32_t)strlen((char *)resp->payload);
    free(json_str);
    cJSON_Delete(arr);
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "get_research_tasks", sizeof(resp->name)-1);
}

void req_get_score(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse((const char *)req->payload);
    char subject[256] = {0};
    int domain = 0;
    if (json) {
        cJSON *s = cJSON_GetObjectItemCaseSensitive(json, "subject");
        cJSON *d = cJSON_GetObjectItemCaseSensitive(json, "domain");
        if (cJSON_IsString(s)) strncpy(subject, s->valuestring, sizeof(subject) - 1);
        if (cJSON_IsNumber(d)) domain = d->valueint;
        cJSON_Delete(json);
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "get_score", sizeof(resp->name) - 1);

    if (strlen(subject) == 0 || domain <= 0) {
        const char *err = "{\"error\": \"subject and domain required\"}";
        strncpy((char *)resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    uint64_t subject_id = djb2_hash(subject);

    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) == MDB_SUCCESS) {
        HyperMemory local_hm = {0};
        local_hm.txn = txn;
        local_hm.dbi_atoms = db.graph.hyper.atoms;
        local_hm.dbi_idx_process = db.graph.hyper.idx_process;
        local_hm.dbi_idx_args = db.graph.hyper.idx_args;
        local_hm.dbi_idx_context = db.graph.hyper.idx_context;

        float score = score_get(&local_hm, (CognitiveDomain)domain, subject_id);
        mdb_txn_abort(txn);

        snprintf((char *)resp->payload, sizeof(resp->payload),
                 "{\"subject\": \"%s\", \"subject_id\": %llu, \"domain\": %d, \"score\": %.4f}",
                 subject, (unsigned long long)subject_id, domain, score);
    } else {
        const char *err = "{\"error\": \"DB transaction failed\"}";
        strncpy((char *)resp->payload, err, sizeof(resp->payload) - 1);
    }
    resp->payload_size = (uint32_t)strlen((char *)resp->payload);
}

void req_get_episodes(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse((const char *)req->payload);
    char subject[256] = {0};
    int limit = 20;
    if (json) {
        cJSON *s = cJSON_GetObjectItemCaseSensitive(json, "subject");
        cJSON *l = cJSON_GetObjectItemCaseSensitive(json, "limit");
        if (cJSON_IsString(s)) strncpy(subject, s->valuestring, sizeof(subject) - 1);
        if (cJSON_IsNumber(l) && l->valueint > 0) limit = l->valueint;
        cJSON_Delete(json);
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "get_episodes", sizeof(resp->name) - 1);

    if (strlen(subject) == 0) {
        const char *err = "{\"error\": \"subject required\"}";
        strncpy((char *)resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    uint64_t subject_id = djb2_hash(subject);
    node_id_t episode_proc = proc_make(djb2_hash("EPISODE_RECORDED"), PROC_KIND_EVENT);

    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) != MDB_SUCCESS) {
        const char *err = "{\"error\": \"DB transaction failed\"}";
        strncpy((char *)resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    HyperMemory local_hm = {0};
    local_hm.txn = txn;
    local_hm.dbi_atoms = db.graph.hyper.atoms;
    local_hm.dbi_idx_process = db.graph.hyper.idx_process;
    local_hm.dbi_idx_args = db.graph.hyper.idx_args;
    local_hm.dbi_idx_context = db.graph.hyper.idx_context;

    NeuroAtom *pointers = NULL;
    size_t count = 0;
    cJSON *arr = cJSON_CreateArray();

    if (hyper_find_by_participant(&local_hm, subject_id, 0, &pointers, &count) == 0) {
        int emitted = 0;
        for (size_t i = 0; i < count && emitted < limit; i++) {
            if (pointers[i].process_id != episode_proc) continue;

            Episode ep;
            if (episode_load(txn, pointers[i].id, &ep) != MDB_SUCCESS) continue;

            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "episode_id", (double)ep.id);
            cJSON_AddNumberToObject(e, "goal_id", (double)ep.goal_id);
            cJSON_AddNumberToObject(e, "algorithm_id", (double)ep.algorithm_id);
            cJSON_AddNumberToObject(e, "result_atom_id", (double)ep.result_atom_id);
            cJSON_AddNumberToObject(e, "vm_status", ep.vm_status);
            cJSON_AddNumberToObject(e, "outcome", (double)ep.outcome);
            cJSON_AddNumberToObject(e, "duration_cycles", (double)ep.duration_cycles);
            cJSON_AddNumberToObject(e, "wall_time", (double)ep.wall_time);
            cJSON_AddItemToArray(arr, e);
            emitted++;
        }
    }
    if (pointers) free(pointers);
    mdb_txn_abort(txn);

    char *json_str = cJSON_PrintUnformatted(arr);
    snprintf((char *)resp->payload, sizeof(resp->payload), "%s", json_str);
    resp->payload_size = (uint32_t)strlen((char *)resp->payload);
    free(json_str);
    cJSON_Delete(arr);
}

void req_get_property(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse((const char *)req->payload);
    uint64_t node_id = 0;
    char key[256] = {0};
    if (json) {
        cJSON *n = cJSON_GetObjectItem(json, "node_id");
        cJSON *subj = cJSON_GetObjectItem(json, "subject");
        cJSON *k = cJSON_GetObjectItem(json, "key");
        if (cJSON_IsNumber(n)) node_id = (uint64_t)n->valuedouble;
        else if (cJSON_IsString(subj)) node_id = djb2_hash(subj->valuestring);
        if (cJSON_IsString(k)) strncpy(key, k->valuestring, sizeof(key) - 1);
        cJSON_Delete(json);
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "get_property", sizeof(resp->name) - 1);

    if (node_id == 0 || key[0] == '\0') {
        const char *err = "{\"error\": \"node_id/subject and key required\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) != MDB_SUCCESS) {
        const char *err = "{\"error\": \"DB transaction failed\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    PropertyType type;
    uint8_t buf[4096];
    uint32_t size = 0;
    int rc = property_get(txn, node_id, key, &type, buf, sizeof(buf), &size);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        const char *err = "{\"error\": \"not found\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    cJSON *root = cJSON_CreateObject();
    switch (type) {
        case PROP_INT: {
            int64_t v; memcpy(&v, buf, sizeof(v));
            cJSON_AddNumberToObject(root, "value", (double)v);
            cJSON_AddStringToObject(root, "type", "int");
            break;
        }
        case PROP_FLOAT: {
            float v; memcpy(&v, buf, sizeof(v));
            cJSON_AddNumberToObject(root, "value", (double)v);
            cJSON_AddStringToObject(root, "type", "float");
            break;
        }
        case PROP_BOOL: {
            bool v; memcpy(&v, buf, sizeof(v));
            cJSON_AddBoolToObject(root, "value", v);
            cJSON_AddStringToObject(root, "type", "bool");
            break;
        }
        case PROP_STRING: {
            char sbuf[4097];
            uint32_t n = size < sizeof(sbuf) - 1 ? size : sizeof(sbuf) - 1;
            memcpy(sbuf, buf, n); sbuf[n] = '\0';
            cJSON_AddStringToObject(root, "value", sbuf);
            cJSON_AddStringToObject(root, "type", "string");
            break;
        }
        default:
            cJSON_AddStringToObject(root, "type", "binary");
            cJSON_AddNumberToObject(root, "size", size);
    }
    mdb_txn_abort(txn);

    char *s = cJSON_PrintUnformatted(root);
    snprintf(resp->payload, sizeof(resp->payload), "%s", s);
    resp->payload_size = (uint32_t)strlen(resp->payload);
    free(s);
    cJSON_Delete(root);
}
