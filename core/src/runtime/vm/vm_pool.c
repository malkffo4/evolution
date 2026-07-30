// runtime/vm/vm_pool.c
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#include "vm_pool.h"
#include "vm.h"
#include "core/globals.h"
#include "storage/db/db.h"
#include "storage/db/db_writer.h"
#include "runtime/logging/logging.h"

typedef struct {
    Pipeline *pipeline;
    HyperMemory *hyper_template; // источник dbi-хендлов, не txn
} VmJob;

// Обёртка: think выполняется в RDONLY, а если внутри пайплайна
// накопились "исходящие" факты (например, через отдельный
// outbox в scratchpad), они пишутся отдельной write-транзакцией.
static int commit_outbox_fn(MDB_txn *txn, void *arg) {
    VMContext *ctx = arg;
    hyper_memory_set_txn(ctx->hyper_mem, txn);
    // здесь — повторный проход по атомам, которые VM пометила
    // как "to_commit" во время RDONLY выполнения (см. ниже).
    // Пока заглушка: реальные hyper_assert_unique уже делают ops сами,
    // если им подсунуть правильный txn.
    return 0;
}

static void *vm_worker(void *arg) {
    VmJob *job = arg;
    MDB_txn *ro_txn;

    if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &ro_txn) != MDB_SUCCESS) {
        LOG_ERROR("vm_pool: failed to open RDONLY txn");
        pipeline_free(job->pipeline);
        free(job);
        return NULL;
    }

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    // --- ФИКС D1: Изолированная рабочая память для каждого потока мышления ---
    WorkingMemory local_wm;
    if (wm_init(&local_wm, 256, 512) != 0) {
        LOG_ERROR("vm_pool: failed to init local working memory");
        mdb_txn_abort(ro_txn);
        return NULL;
    }

    // Инициализируем ВМ с ЛОКАЛЬНОЙ памятью, а не global_wm
    vm_init(&ctx, ro_txn, &local_wm);

    ctx.hyper_mem = job->hyper_template;
    hyper_memory_set_txn(ctx.hyper_mem, ro_txn);

    int rc = vm_execute(&ctx, job->pipeline);

    mdb_txn_abort(ro_txn); // RDONLY транзакции просто абортим

    if (rc == VM_OK) {
        // Коммитим исходящие знания (outbox), если гипотеза выжила
        db_write_sync(commit_outbox_fn, &ctx);
    }

    // Очищаем ВМ и её изолированную рабочую память
    vm_destroy(&ctx);
    wm_clear(&local_wm);
    pipeline_free(job->pipeline);
    free(job);

    return NULL;
}

void vm_pool_submit(Pipeline *pipeline, HyperMemory *hmem, WorkingMemory *wm) {
    (void)wm;
    VmJob *job = malloc(sizeof(VmJob));
    job->pipeline = pipeline;
    job->hyper_template = hmem;

    pthread_t t;
    pthread_create(&t, NULL, vm_worker, job);
    pthread_detach(t);
}
