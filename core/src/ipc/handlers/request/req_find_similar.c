// ipc/handlers/request/req_find_similar.c
//
// Новый файл — Makefile собирает src/**/*.c автоматически
// (SRC_LIB := $(shell find src -name '*.c')), править Makefile не нужно.
//
// Единственная задача: дать Python способ вызвать УЖЕ СУЩЕСТВУЮЩИЙ
// find_similar_nodes() (storage/vector_store/vector_store.c) снаружи.
// Внутренности simhash_index/LSH здесь не меняются — переделка
// индекса под настоящий LSH banding сознательно отложена (следующий
// приоритет), это чисто транспортный хэндлер поверх того, что есть.
//
// Транзакции: MDB_RDONLY, открывается и сразу закрывается прямо в
// IPC-потоке — тот же паттерн, что req_retrieve/req_get_episodes.
// db_writer здесь не участвует вообще: чтение векторов не требует
// эксклюзивного писателя.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <cjson/cJSON.h>

#include "ipc/ipc.h"
#include "storage/db/db.h"
#include "storage/vector_store/vector_store.h"   // find_similar_nodes(), VECTOR_DIM
#include "storage/string_pool/string_pool.h"      // get_string_from_pool()

#define REQ_FIND_SIMILAR_MAX_TOPK 32

void req_find_similar(IPCPacket *req, IPCPacket *resp) {
    cJSON *json = cJSON_Parse((const char *)req->payload);

    static float query[VECTOR_DIM];
    memset(query, 0, sizeof(query));
    bool have_vec = false;
    int top_k = 8;

    if (json) {
        cJSON *emb = cJSON_GetObjectItem(json, "embedding");
        cJSON *k   = cJSON_GetObjectItem(json, "top_k");

        if (cJSON_IsArray(emb) && cJSON_GetArraySize(emb) == VECTOR_DIM) {
            for (int i = 0; i < VECTOR_DIM; i++) {
                cJSON *item = cJSON_GetArrayItem(emb, i);
                query[i] = cJSON_IsNumber(item) ? (float)item->valuedouble : 0.0f;
            }
            have_vec = true;
        }
        if (cJSON_IsNumber(k) && k->valueint > 0 && k->valueint <= REQ_FIND_SIMILAR_MAX_TOPK)
            top_k = k->valueint;

        cJSON_Delete(json);
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "find_similar", sizeof(resp->name) - 1);

    if (!have_vec) {
        const char *err = "{\"error\": \"'embedding' must be an array of VECTOR_DIM (128) floats\"}";
        strncpy((char *)resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &txn) != MDB_SUCCESS) {
        const char *err = "{\"error\": \"DB transaction failed\"}";
        strncpy((char *)resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    uint64_t results[REQ_FIND_SIMILAR_MAX_TOPK];
    int count = find_similar_nodes(txn, query, top_k, results);
    if (count < 0) count = 0;

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "results");

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();

        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)results[i]);
        cJSON_AddStringToObject(item, "id", idbuf);

        // Строка попадает в string pool при ingestion через
        // add_string_to_pool() в perception.c::resolve_arg — тот же
        // путь, которым retrieve() уже резолвит человекочитаемые
        // метки. Если атом создавался без явного "id"/строкового
        // args, label будет пуст — Python-сторона (retrieval.py) это
        // переживает молча, просто пропускает соседа.
        const char *label = get_string_from_pool(txn, results[i]);
        cJSON_AddStringToObject(item, "label", label ? label : "");

        cJSON_AddItemToArray(arr, item);
    }

    mdb_txn_abort(txn);

    char *s = cJSON_PrintUnformatted(root);
    snprintf((char *)resp->payload, sizeof(resp->payload), "%s", s);
    resp->payload_size = (uint32_t)strlen((char *)resp->payload);
    free(s);
    cJSON_Delete(root);
}
