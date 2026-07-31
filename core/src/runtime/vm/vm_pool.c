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

typedef struct {
    Pipeline    *pipeline;
    node_id_t    goal_id;
    node_id_t    algo_id;
    VMStatus     result;
} VmJob;

static int vm_worker_txn_fn(MDB_txn *txn, void *arg) {
    VmJob *job = arg;

    // 1. Инициализируем локальную Working Memory
    WorkingMemory local_wm;
    if (wm_init(&local_wm, 256, 512) != 0) {
        LOG_ERROR("vm_pool: failed to init local working memory");
        return -1;
    }

    // 2. Инициализируем полностью ИЗОЛИРОВАННУЮ HyperMemory для воркера
    HyperMemory *worker_hmem = hyper_memory_new(txn,
        db.graph.hyper.atoms,
        db.graph.hyper.idx_process,
        db.graph.hyper.idx_args,
        db.graph.hyper.idx_context);
    
    if (!worker_hmem) {
        wm_clear(&local_wm);
        return -1;
    }
    
    // ЗАЩИТА ОТ КОЛЛИЗИЙ ID: Поскольку каждый воркер имеет свой собственный
    // счётчик ID, начинающийся с 1, мы задаём случайный session_id, чтобы
    // гарантировать, что созданные атомы (Score, Episode) не затрут друг друга.
    worker_hmem->idgen->session_id = (uint16_t)(vm_rdtsc() & 0xFFFF);
    
    hyper_memory_set_db_causal(worker_hmem, db.graph.hyper.idx_causal);
    hyper_memory_set_db_archive(worker_hmem, db.graph.hyper.archive);
    hyper_memory_set_db_vectors(worker_hmem, db.graph.hyper.idx_vectors);

    // 3. Создаем контекст выполнения
    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (vm_init(&ctx, txn, &local_wm) != VM_OK) {
        LOG_ERROR("vm_pool: vm_init failed");
        hyper_memory_free(worker_hmem);
        wm_clear(&local_wm);
        return -1;
    }

    ctx.hyper_mem = worker_hmem;

    // Замеряем время для эпизода
    uint64_t t_start = vm_rdtsc();
    int rc = vm_execute(&ctx, job->pipeline);
    uint64_t t_end = vm_rdtsc();
    
    job->result = (VMStatus)rc;

    // 4. Записываем результаты обучения (Score и Episode)
    float outcome = (rc == VM_OK) ? 1.0f : 0.0f;
    score_update(ctx.hyper_mem, COGNITIVE_DOMAIN_ALGORITHM, job->algo_id, outcome, 0, 0);

    if (ctx.last_result_id != 0) {
        score_propagate_credit(ctx.hyper_mem, COGNITIVE_DOMAIN_HYPOTHESIS,
            ctx.last_result_id, outcome, 0, 0.7f);
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
    episode_record(ctx.hyper_mem, &ep);

    vm_destroy(&ctx);
    hyper_memory_free(worker_hmem);
    wm_clear(&local_wm);

    return (rc == VM_OK) ? 0 : -1;
}

static void *vm_worker(void *arg) {
    pthread_detach(pthread_self());
    VmJob *job = arg;

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

    pthread_t t;
    if (pthread_create(&t, NULL, vm_worker, job) != 0) {
        LOG_ERROR("vm_pool: pthread_create failed");
        pipeline_free(job->pipeline);
        free(job);
    }
}

void vm_pool_init(void) {}
void vm_pool_shutdown(void) {}