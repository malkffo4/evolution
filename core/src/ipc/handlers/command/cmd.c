// ipc/handlers/command/cmd.c
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <lmdb.h>
#include <cjson/cJSON.h>

#include "core/globals.h"
#include "core/message_bus.h"
#include "ipc/router_handlers.h"
#include "ipc/ipc.h"
#include "knowledge/algorithm_saver.h"
#include "knowledge/pipeline_io.h"
#include "perception/perception.h"
#include "memory/working.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/logging/logging.h"
#include "storage/db/db.h"
#include "storage/db/db_writer.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "storage/hyper_atom/hyper_pattern.h"

typedef struct {
    IPCPacket *req;
    uint64_t   algo_id;
    Pipeline  *imported_pipeline;
    bool       is_pattern;
    HyperPattern pattern;
} LearnJob;

static int learn_txn_fn(MDB_txn *txn, void *arg) {
    LearnJob *job = arg;

    if (job->imported_pipeline)
        return algorithm_save(txn, job->algo_id, job->imported_pipeline) == MDB_SUCCESS ? 0 : -1;

    if (job->is_pattern)
        return hyper_pattern_save(txn, db.graph.hyper.patterns, &job->pattern) == MDB_SUCCESS ? 0 : -1;

    cJSON *probe = cJSON_Parse(job->req->payload);
    bool has_atoms = probe && cJSON_HasObjectItem(probe, "atoms");
    bool has_nodes = probe && cJSON_HasObjectItem(probe, "nodes");
    if (probe) cJSON_Delete(probe);

    if (has_atoms)
        return perceive_hyper_json(job->req->payload, txn, global_hyper_mem) == 0 ? 0 : -1;
    if (has_nodes)
        return perceive_and_activate(job->req->payload, &global_wm, txn, global_hyper_mem) == 0 ? 0 : -1;

    return -1;
}

void cmd_learn(IPCPacket *req, IPCPacket *resp) {
    LearnJob job = { .req = req };
    cJSON *root = cJSON_Parse((char *)req->payload);
    if (root) {
        cJSON *type = cJSON_GetObjectItem(root, "type");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "pipeline") == 0) {
            job.imported_pipeline = pipeline_from_json(root, &job.algo_id);
        } else if (cJSON_IsString(type) && strcmp(type->valuestring, "hyper_pattern") == 0) {
            job.is_pattern = (hyper_pattern_from_json(root, &job.pattern) == 0);
        }
        cJSON_Delete(root);
    }

    int rc = db_write_sync(learn_txn_fn, &job);
    if (job.imported_pipeline) pipeline_free(job.imported_pipeline);

    resp->type = IPC_RESPONSE;
    if (rc == 0) {
        snprintf(resp->name, sizeof(resp->name), "learn");
        snprintf((char *)resp->payload, sizeof(resp->payload), "{\"ok\": true}");
    } else {
        snprintf(resp->name, sizeof(resp->name), "error");
        snprintf((char *)resp->payload, sizeof(resp->payload), "Learn failed");
    }
    resp->payload_size = (uint32_t)strlen(resp->payload);
}

void cmd_shutdown(IPCPacket *req, IPCPacket *resp) {
    LOG_INFO("Get command for Shutting down...");
    (void)req;

    resp->type = IPC_RESPONSE;
    snprintf(resp->name, sizeof(resp->name), "shutdown");
    snprintf(resp->payload, sizeof(resp->payload), "{\"ok\": true}");
    resp->payload_size = (uint32_t)strlen(resp->payload);

    // УСТРАНЕНИЕ БАГА: Выставляем флаг остановки и будим спящие шины.
    // Больше никакой деструктивной логики и pthread_join() внутри самого сетевого потока!
    g_running = 0;
    bus_stop();
}

void cmd_think(IPCPacket *req, IPCPacket *resp) {
    (void)req;

    // Принудительно дёргаем MainLoop вручную (без транзакции LMDB — демон сам управляет)
    // Для этого просто отправляем событие пробуждения демону,
    // либо, если у вас есть функция ручного запуска MainLoop, вызываем её.

    // Простейший вариант: ставим флаг, который демон проверяет.
    // Пока что просто отвечаем OK, что триггернуло демон.
    extern int g_think_trigger;  // объявим в subconscious.c
    g_think_trigger = 1;

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "think", sizeof(resp->name)-1);
    const char* ok_msg = "{\"ok\": true, \"msg\": \"MainLoop triggered\"}";
    strncpy(resp->payload, ok_msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(ok_msg);
}
