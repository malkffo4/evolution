// ipc/handlers/command/cmd.c
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <lmdb.h>
#include <cjson/cJSON.h>
#include <time.h>

#include "core/globals.h"
#include "core/message_bus.h"
#include "ipc/router_handlers.h"
#include "ipc/ipc.h"
#include "knowledge/algorithm_saver.h"
#include "knowledge/pipeline_io.h"
#include "perception/perception.h"
#include "memory/working.h"
#include "memory/subconscious.h"
#include "runtime/vm/vm_pool.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/logging/logging.h"
#include "storage/db/db.h"
#include "storage/db/db_writer.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "storage/hyper_atom/hyper_pattern.h"
#include "storage/property/property.h"
#include "math/hash.h"
#include "reasoning/algorithm_planner.h"
#include "reasoning/planner.h"

typedef struct {
    IPCPacket *req;
    uint64_t   algo_id;
    Pipeline  *imported_pipeline;
    bool       is_pattern;
    HyperPattern pattern;
} LearnJob;

typedef struct { ko_id_t ids[64]; int count; } FlawJob;

typedef struct { uint64_t goal_id; } ClearCooldownJob;

static int mark_flaw_txn_fn(MDB_txn *txn, void *arg) {
    FlawJob *job = arg;
    HyperMemory *hmem = hyper_memory_new(db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    if (!hmem) return -1;

    node_id_t flaw_proc = proc_make(djb2_hash("HAS_FLAW"), PROC_KIND_RELATION);
    node_id_t garbage_concept = djb2_hash("GarbageCandidate");

    for (int i = 0; i < job->count; i++) {
        NeuroAtom flaw = {0};
        flaw.id = hyper_memory_new_id(hmem);
        flaw.process_id = flaw_proc;
        // Прямая REF-ссылка на УЖЕ СУЩЕСТВУЮЩИЙ атом по его реальному ko_id_t
        // (не через djb2 от строки — числовой ID через generic "learn"-путь
        // трактуется как HYPER_TYPE_INT, а не REF; см. perception.c::resolve_arg).
        flaw.args[0].raw = HYPER_MAKE_REF(job->ids[i]);
        flaw.args[1].raw = HYPER_MAKE_REF(garbage_concept);
        flaw.truth_mean = 1.0f;
        flaw.truth_confidence = 0.9f;
        flaw.sti = 0.05f;  // сама пометка не засоряет активное внимание
        flaw.lti = 0.30f;  // но переживёт несколько decay-тиков до ручного/Critic-review
        hyper_assert_unique(txn, hmem, &flaw);
    }
    hyper_memory_free(hmem);
    return 0;
}

void cmd_mark_flaw(IPCPacket *req, IPCPacket *resp) {
    cJSON *root = cJSON_Parse((const char *)req->payload);
    FlawJob job = {0};

    if (root) {
        cJSON *ids = cJSON_GetObjectItem(root, "atom_ids");
        if (cJSON_IsArray(ids)) {
            int n = cJSON_GetArraySize(ids);
            for (int i = 0; i < n && i < 64; i++) {
                cJSON *item = cJSON_GetArrayItem(ids, i);
                if (cJSON_IsNumber(item)) job.ids[job.count++] = (ko_id_t)item->valuedouble;
            }
        }
        cJSON_Delete(root);
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "mark_flaw", sizeof(resp->name) - 1);

    if (job.count == 0) {
        const char *err = "{\"error\": \"empty atom_ids\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }

    int rc = db_write_sync(mark_flaw_txn_fn, &job);
    snprintf(resp->payload, sizeof(resp->payload),
             rc == 0 ? "{\"ok\": true, \"flagged\": %d}" : "{\"error\": \"mark_flaw txn failed\"}",
             job.count);
    resp->payload_size = (uint32_t)strlen(resp->payload);
}

static int learn_txn_fn(MDB_txn *txn, void *arg) {
    LearnJob *job = arg;

    if (job->imported_pipeline) {
        int rc = algorithm_save(txn, job->algo_id, job->imported_pipeline);
        if (rc == MDB_SUCCESS) {
            // ФИКС: Спасаем регистровые контракты, так как algorithm_save их теряет.
            // Иначе ZeroShotComposer не сможет вставить нужный MOVE
            if (job->imported_pipeline->in_count > 0) {
                int64_t v = job->imported_pipeline->in_regs[0];
                property_set(txn, job->algo_id, "input_reg", PROP_INT, &v, sizeof(v));
            }
            if (job->imported_pipeline->out_count > 0) {
                int64_t v = job->imported_pipeline->out_regs[0];
                property_set(txn, job->algo_id, "output_reg", PROP_INT, &v, sizeof(v));
            }
            return 0;
        }
        return -1;
    }

    if (job->is_pattern)
        return hyper_pattern_save(txn, db.graph.hyper.patterns, &job->pattern) == MDB_SUCCESS ? 0 : -1;

    // Дешёвая инвалидация кэша find_goal_algorithm_relations() (см. 3.2):
    // мета-факт GoalAlgorithmRelation меняется исключительно через "learn".
    if (strstr(job->req->payload, "GoalAlgorithmRelation"))
        invalidate_goal_algorithm_relation_cache();

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
            if (job.imported_pipeline) {
                cJSON *in_regs = cJSON_GetObjectItem(root, "in_regs");
                if (cJSON_IsArray(in_regs)) {
                    int n = cJSON_GetArraySize(in_regs);
                    for (int i = 0; i < n && i < 8; i++) {
                        cJSON *item = cJSON_GetArrayItem(in_regs, i);
                        if (cJSON_IsNumber(item))
                            job.imported_pipeline->in_regs[job.imported_pipeline->in_count++] = (uint8_t)item->valueint;
                    }
                }
                cJSON *out_regs = cJSON_GetObjectItem(root, "out_regs");
                if (cJSON_IsArray(out_regs)) {
                    int n = cJSON_GetArraySize(out_regs);
                    for (int i = 0; i < n && i < 8; i++) {
                        cJSON *item = cJSON_GetArrayItem(out_regs, i);
                        if (cJSON_IsNumber(item))
                            job.imported_pipeline->out_regs[job.imported_pipeline->out_count++] = (uint8_t)item->valueint;
                    }
                }
            }
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

    int ticks = 0;
    while (subconscious_force_tick() == 1 && ticks < 100) {
        ticks++;
    }

    extern volatile int g_think_trigger;
    g_think_trigger = 1;

    // Дренаж: MainLoop мог за эти тики диспетчеризовать асинхронную работу
    // (OP_DISPATCH_ASYNC -> vm_pool). Архитектура остаётся асинхронной —
    // dmn_loop продолжает тикать независимо — но IPC-ответ на "think" теперь
    // осмысленно говорит "результат этой волны уже виден в LMDB", а не
    // "тик поставлен в очередь, но неизвестно когда выполнится".
    int waited_ms = 0;
    const int poll_ms = 20, max_wait_ms = 3000;
    while (vm_pool_active_count() > 0 && waited_ms < max_wait_ms) {
        struct timespec ts = { 0, poll_ms * 1000000L };
        nanosleep(&ts, NULL);
        waited_ms += poll_ms;
    }

    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "think", sizeof(resp->name)-1);

    char ok_msg[160];
    snprintf(ok_msg, sizeof(ok_msg),
             "{\"ok\": true, \"msg\": \"MainLoop executed %d ticks\", "
             "\"drained\": %s, \"waited_ms\": %d}",
             ticks, (vm_pool_active_count() == 0) ? "true" : "false", waited_ms);
    strncpy(resp->payload, ok_msg, sizeof(resp->payload)-1);
    resp->payload_size = (uint32_t)strlen(ok_msg);
}

static int clear_cooldown_txn_fn(MDB_txn *txn, void *arg) {
    ClearCooldownJob *job = arg;
    int64_t zero = 0;
    return property_set(txn, job->goal_id, "cooldown_until", PROP_INT,
                         &zero, sizeof(zero)) == MDB_SUCCESS ? 0 : -1;
}

void cmd_clear_cooldown(IPCPacket *req, IPCPacket *resp) {
    cJSON *root = cJSON_Parse((const char *)req->payload);
    resp->type = IPC_RESPONSE;
    strncpy(resp->name, "clear_cooldown", sizeof(resp->name) - 1);

    if (!root) {
        const char *err = "{\"error\": \"invalid JSON\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
        resp->payload_size = (uint32_t)strlen(err);
        return;
    }
    cJSON *goal_json = cJSON_GetObjectItem(root, "goal");
    if (cJSON_IsString(goal_json)) {
        ClearCooldownJob job = { .goal_id = djb2_hash(goal_json->valuestring) };

        clear_goal_cooldown(job.goal_id);   // legacy-массив — на всякий случай синхронизируем тоже

        int rc = db_write_sync(clear_cooldown_txn_fn, &job);
        if (rc == 0) {
            snprintf(resp->payload, sizeof(resp->payload),
                     "{\"ok\": true, \"goal_id\": %llu}", (unsigned long long)job.goal_id);
        } else {
            snprintf(resp->payload, sizeof(resp->payload),
                     "{\"error\": \"failed to reset cooldown_until property\"}");
        }
    } else {
        const char *err = "{\"error\": \"missing 'goal'\"}";
        strncpy(resp->payload, err, sizeof(resp->payload) - 1);
    }
    cJSON_Delete(root);
    resp->payload_size = (uint32_t)strlen(resp->payload);
}
