// ipc/handlers/command/cmd_execute.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "ipc/ipc.h"
#include "storage/db/db.h"
#include "storage/db/db_writer.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "storage/string_pool/string_pool.h"
#include "memory/working.h"
#include "core/globals.h"
#include "math/hash.h"
#include "runtime/logging/logging.h"

typedef struct {
    char      goal_name[256];
    float     utility;
    node_id_t goal_id; // выход
} GoalActivationJob;

// Выполняется ИСКЛЮЧИТЕЛЬНО внутри write-транзакции потока db_writer.
// IPC-поток никогда не открывает собственную write-транзакцию — это
// гарантирует единственность писателя LMDB на весь процесс.
static int activate_goal_txn_fn(MDB_txn *txn, void *arg) {
    GoalActivationJob *job = arg;

    node_id_t goal_id = djb2_hash(job->goal_name);
    job->goal_id = goal_id;

    // Регистрируем читаемое имя цели в строковом пуле — используется позже
    // Research Engine'ом через get_string_from_pool() при VM_NOT_FOUND
    // (см. runtime/ops/cognitive.c::vm_op_evaluate_goals).
    add_string_to_pool(txn, job->goal_name);

    HyperMemory *hmem = hyper_memory_new(txn,
        db.graph.hyper.atoms,
        db.graph.hyper.idx_process,
        db.graph.hyper.idx_args,
        db.graph.hyper.idx_context);
    if (!hmem) return -1;
    hyper_memory_set_db_causal(hmem, db.graph.hyper.idx_causal);

    // Knowledge Object минимально должен иметь тип
    // (docs/03_Knowledge.md: "Тип определяет семантику объекта").
    NeuroAtom type_atom = {0};
    type_atom.id          = hyper_memory_new_id(hmem);
    type_atom.process_id  = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
    type_atom.args[0].raw = HYPER_MAKE_REF(goal_id);
    type_atom.args[1].raw = HYPER_MAKE_REF(djb2_hash("Goal"));
    type_atom.truth_mean       = 1.0f;
    type_atom.truth_confidence = 1.0f;
    type_atom.sti = 0.5f;
    type_atom.lti = 0.2f;
    int rc = hyper_assert_unique(hmem, &type_atom);
    hyper_memory_free(hmem);
    if (rc < 0) return -1;

    // Активация Global Workspace — единственное место, где IPC касается
    // WorkingMemory. Само рассуждение выполнит MainLoop асинхронно.
    wm_activate(&global_wm, goal_id, 1.0f, job->utility);
    wm_wrlock(&global_wm);
    for (uint32_t i = 0; i < global_wm.count; i++) {
        if (global_wm.nodes[i].node_id == goal_id) {
            global_wm.nodes[i].state.usefulness = job->utility;
            break;
        }
    }
    wm_unlock(&global_wm);

    return 0;
}

void cmd_execute_op(IPCPacket *req, IPCPacket *resp) {
    GoalActivationJob job = {0};
    job.utility = 0.9f;

    cJSON *root = cJSON_Parse((const char *)req->payload);
    if (root) {
        cJSON *goal_json = cJSON_GetObjectItem(root, "goal");
        if (cJSON_IsString(goal_json) && goal_json->valuestring) {
            strncpy(job.goal_name, goal_json->valuestring, sizeof(job.goal_name) - 1);
        }
        cJSON *utility_json = cJSON_GetObjectItem(root, "utility");
        if (cJSON_IsNumber(utility_json)) {
            float u = (float)utility_json->valuedouble;
            if (u < 0.0f) u = 0.0f;
            if (u > 1.0f) u = 1.0f;
            job.utility = u;
        }
        cJSON_Delete(root);
    }

    resp->type = IPC_RESPONSE;
    snprintf(resp->name, sizeof(resp->name), "execute_op");

    if (job.goal_name[0] == '\0') {
        snprintf(resp->payload, sizeof(resp->payload),
                 "{\"error\": \"missing required field 'goal'\"}");
        resp->payload_size = (uint32_t)strlen(resp->payload);
        return;
    }

    int rc = db_write_sync(activate_goal_txn_fn, &job);
    if (rc != 0) {
        snprintf(resp->payload, sizeof(resp->payload),
                 "{\"error\": \"failed to activate goal in working memory\"}");
    } else {
        snprintf(resp->payload, sizeof(resp->payload),
                 "{\"status\": \"queued_in_working_memory\", \"goal_id\": %llu}",
                 (unsigned long long)job.goal_id);
    }
    resp->payload_size = (uint32_t)strlen(resp->payload);
}