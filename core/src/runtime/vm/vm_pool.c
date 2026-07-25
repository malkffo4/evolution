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
    vm_init(&ctx, ro_txn, NULL /* своя изолированная WM, если нужна */);

    // ВАЖНО: hyper_mem с этим RO-txn можно только читать (OP_QUERY, OP_TRACE).
    // OP_ASSERT/OP_DERIVE внутри "мышления" должны не писать в LMDB сразу,
    // а копить результат в ctx.scratchpad/arena — и уже после того как
    // vm_execute() вернул VM_OK, мы одной db_write_sync() переносим
    // накопленное в базу.
    ctx.hyper_mem = global_hyper_mem; // dbi-хендлы общие, txn переставим ниже
    hyper_memory_set_txn(ctx.hyper_mem, ro_txn);

    int rc = vm_execute(&ctx, job->pipeline);

    mdb_txn_abort(ro_txn); // RDONLY транзакции просто абортим, не коммитим

    if (rc == VM_OK) {
        // Если гипотеза "выжила" — коммитим её одним write-переходом
        db_write_sync(commit_outbox_fn, &ctx);
    }

    vm_destroy(&ctx);
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
