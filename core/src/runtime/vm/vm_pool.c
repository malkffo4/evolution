// runtime/vm/vm_pool.c
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "vm_context.h"
#include "vm_pool.h"
#include "vm.h"
#include "core/globals.h"
#include "storage/db/db.h"
#include "storage/db/db_writer.h"
#include "runtime/logging/logging.h"
#include "runtime/time/time.h"
#include "knowledge/evaluation.h"
#include "knowledge/episode.h"
#include "reasoning/strategy_store.h"

typedef struct {
    Pipeline    *pipeline;
    node_id_t    goal_id;
    node_id_t    algo_id;
    VMStatus     result;
} VmJob;

static int vm_worker_txn_fn(MDB_txn *txn, void *arg) {
    VmJob *job = arg;

    LOG_INFO("[VM_POOL] worker txn started: algo=%lu goal=%lu",
             (unsigned long)job->algo_id, (unsigned long)job->goal_id);

    WorkingMemory local_wm;
    if (wm_init(&local_wm, 256, 512) != 0) {
        LOG_ERROR("[VM_POOL] wm_init failed: algo=%lu goal=%lu",
                  (unsigned long)job->algo_id, (unsigned long)job->goal_id);
        return -1; // инфраструктурная ошибка — abort оправдан
    }

    HyperMemory *worker_hmem = hyper_memory_new(txn,
        db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    if (!worker_hmem) {
        LOG_ERROR("[VM_POOL] hyper_memory_new failed: algo=%lu", (unsigned long)job->algo_id);
        wm_clear(&local_wm);
        return -1;
    }

    // Случайный session_id — защита от коллизий ID между параллельными воркерами.
    worker_hmem->idgen->session_id = (uint16_t)(vm_rdtsc() & 0xFFFF);
    hyper_memory_set_db_causal(worker_hmem, db.graph.hyper.idx_causal);
    hyper_memory_set_db_archive(worker_hmem, db.graph.hyper.archive);
    hyper_memory_set_db_vectors(worker_hmem, db.graph.hyper.idx_vectors);

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (vm_init(&ctx, txn, &local_wm) != VM_OK) {
        LOG_ERROR("[VM_POOL] vm_init failed: algo=%lu", (unsigned long)job->algo_id);
        hyper_memory_free(worker_hmem);
        wm_clear(&local_wm);
        return -1;
    }

    // Инъекция цели напрямую в ��окальную рабочую память воркера,
    // чтобы алгоритмы, опирающиеся на OP_WM_TOP_GOAL (например, InductiveExtractor
    // или AnalogyPlanner), могли ее обнаружить.
    if (job->goal_id != 0) {
        wm_activate(&local_wm, job->goal_id, 1.0f, 1.0f);
        for (uint32_t i = 0; i < local_wm.count; i++) {
            if (local_wm.nodes[i].node_id == job->goal_id) {
                local_wm.nodes[i].state.usefulness = 1.0f;
                break;
            }
        }
    }

    ctx.hyper_mem = worker_hmem;

    uint64_t t_start = vm_rdtsc();
    int rc = vm_execute(&ctx, job->pipeline);
    uint64_t t_end = vm_rdtsc();
    job->result = (VMStatus)rc;

    // ДИАГНОСТИКА: подробный лог причины провала — регистр IP, глубина
    // стека фреймов, накопленные циклы, последний выведенный атом.
    if (rc != VM_OK) {
        LOG_ERROR("[VM_POOL] Algorithm FAILED: algo_id=%lu goal_id=%lu vm_status=%d "
                  "frame=%u ip=%u cycles=%lu last_result_id=%lu",
                  (unsigned long)job->algo_id, (unsigned long)job->goal_id, rc,
                  ctx.frame, ctx.frames[ctx.frame].ip,
                  (unsigned long)ctx.cycles, (unsigned long)ctx.last_result_id);
    }

    // ГЛАВНЫЙ ФИКС: неудача исполнения — валидный когнитивный опыт
    // (docs/09_Learning.md: "Обучение на ошибках"), а не инфраструктурный
    // сбой. Score/Episode об ошибке ОБЯЗАНЫ попасть в LMDB — иначе UCB
    // (algorithm_planner.c::pick_best) никогда не увидит, что confidence
    // упала, и будет бесконечно выбирать один и тот же плохой алгоритм.
    float outcome = (rc == VM_OK) ? 1.0f : 0.0f;
    if (score_update(ctx.hyper_mem, COGNITIVE_DOMAIN_ALGORITHM, job->algo_id, outcome, 0, 0) != 0) {
        LOG_ERROR("[VM_POOL] score_update failed: algo=%lu outcome=%.1f",
                  (unsigned long)job->algo_id, outcome);
    }

    if (ctx.last_result_id != 0) {
        int propagated = score_propagate_credit(ctx.hyper_mem, COGNITIVE_DOMAIN_HYPOTHESIS,
                                                  ctx.last_result_id, outcome, 0, 0.7f);
        // Если гипотеза была построена по аналогии (ctx.userdata хранит признаки x[4],
        // проставленные в момент OP_DERIVE аналогии — см. AnalogyEvaluation.score),
        // используем реальный исход как обучающий сигнал для весов эвристики.
        if (ctx.userdata) {
            const float *analogy_features = (const float *)ctx.userdata; // [neigh,center,cov,rel]
            reasoning_weights_sgd_update(txn, analogy_features, outcome);
        }
        LOG_DEBUG("[VM_POOL] credit propagated=%d result_id=%lu", propagated,
                (unsigned long)ctx.last_result_id);
    }

    Episode ep = {0};
    ep.id               = hyper_memory_new_id(ctx.hyper_mem);
    ep.goal_id          = job->goal_id;
    ep.algorithm_id     = job->algo_id;
    ep.result_atom_id   = ctx.last_result_id;
    ep.context_id       = ctx.current_context;
    ep.vm_status        = (int32_t)rc;
    ep.outcome          = outcome;
    ep.start_cycles     = t_start;
    ep.duration_cycles  = (t_end > t_start) ? (t_end - t_start) : 0;
    ep.wall_time        = (uint64_t)time(NULL);
    if (episode_record(ctx.hyper_mem, &ep) != 0) {
        LOG_ERROR("[VM_POOL] episode_record failed: algo=%lu goal=%lu",
                  (unsigned long)job->algo_id, (unsigned long)job->goal_id);
    }

    vm_destroy(&ctx);
    hyper_memory_free(worker_hmem);
    wm_clear(&local_wm);

    // Всегда 0 (commit): и успех, и провал алгоритма — персистентный опыт.
    return 0;
}

static void *vm_worker(void *arg) {
    pthread_detach(pthread_self());
    VmJob *job = arg;

    LOG_INFO("[VM_POOL] worker thread started for algo=%lu goal=%lu",
             (unsigned long)job->algo_id, (unsigned long)job->goal_id);

    int rc = db_write_sync(vm_worker_txn_fn, job);
    if (rc != 0) {
        LOG_DEBUG("vm_pool: worker finished non-OK (vm_status=%d, txn_rc=%d)",
                  job->result, rc);
    }

    pipeline_free(job->pipeline);
    free(job);
    return NULL;
}

void vm_pool_submit(Pipeline *pipeline, node_id_t goal_id, node_id_t algo_id) {
    if (!pipeline) {
        LOG_ERROR("vm_pool_submit: invalid arguments");
        return;
    }

    VmJob *job = malloc(sizeof(VmJob));
    if (!job) {
        LOG_ERROR("vm_pool: OOM allocating job");
        pipeline_free(pipeline);
        return;
    }

    job->pipeline       = pipeline;
    job->goal_id        = goal_id;
    job->algo_id        = algo_id;
    job->result         = VM_ERROR;

    LOG_DEBUG("[VM_POOL] submit goal=%lu algo=%lu", (unsigned long)goal_id, (unsigned long)algo_id);

    pthread_t t;
    if (pthread_create(&t, NULL, vm_worker, job) != 0) {
        LOG_ERROR("vm_pool: pthread_create failed");
        pipeline_free(job->pipeline);
        free(job);
    }
}

void vm_pool_init(void) {}
void vm_pool_shutdown(void) {}
