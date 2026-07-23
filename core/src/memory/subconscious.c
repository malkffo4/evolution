// memory/subconscious.c
#include <stddef.h>
#include <stdint.h>
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "main.h"
#include "core/globals.h"
#include "subconscious.h"
#include "storage/db/db.h"
#include "storage/string_pool/string_pool.h"
#include "memory/working.h"
#include "reasoning/planner.h"
#include "knowledge/algorithm_loader.h"
#include "knowledge/algorithm_saver.h"
#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/ops/opcode.h"
#include "runtime/logging/logging.h"
#include "math/hash.h"

static ResearchTask task_queue[MAX_PENDING_TASKS];
static int task_count = 0;
static pthread_mutex_t task_mutex = PTHREAD_MUTEX_INITIALIZER;

void enqueue_research_task(uint64_t node_id, const char *query) {
    pthread_mutex_lock(&task_mutex);
    if (task_count < MAX_PENDING_TASKS) {
        task_queue[task_count].node_id = node_id;
        strncpy(task_queue[task_count].query, query, sizeof(task_queue[task_count].query) - 1);
        task_count++;
    }
    pthread_mutex_unlock(&task_mutex);
}

int get_pending_tasks(ResearchTask *buffer, int max_count) {
    int cnt = 0;
    pthread_mutex_lock(&task_mutex);
    cnt = (task_count < max_count) ? task_count : max_count;
    memcpy(buffer, task_queue, (size_t)cnt * sizeof(ResearchTask));
    if (cnt > 0) {
        memmove(task_queue, task_queue + cnt, (size_t)(task_count - cnt) * sizeof(ResearchTask));
        task_count -= cnt;
    }
    pthread_mutex_unlock(&task_mutex);
    return cnt;
}

static int dmn_running = 0;
static pthread_t dmn_thread;
static WorkingMemory *global_wm = NULL;
static uint64_t main_loop_algo_id = 0;

static Pipeline* build_main_loop_pipeline(void) {
    Pipeline *p = calloc(1, sizeof(Pipeline));
    static Instruction code[] = {
        { .operator_id = OP_SPREAD_ACTIVATION },
        { .operator_id = OP_EVALUATE_GOALS },
        // Здесь можно добавить OP_QUERY для гипер-целей
        { .operator_id = OP_HALT }
    };
    p->code_len = sizeof(code)/sizeof(Instruction);
    p->capacity = p->code_len;
    p->code = malloc(sizeof(code));
    memcpy(p->code, code, sizeof(code));
    p->constants.int_consts = NULL;
    p->constants.int_count = 0;
    return p;
}

// Инициализация: сохраняем MainLoop алгоритм в БД, если его нет
static void ensure_main_loop_exists(MDB_txn *txn) {
    main_loop_algo_id = djb2_hash("MainLoop");

    // Проверяем, есть ли уже алгоритм
    Pipeline *existing = NULL;
    if (algorithm_load(txn, main_loop_algo_id, &existing) == 0) {
        if (existing) {
            free(existing->code);
            free(existing);
        }
        return; // уже есть
    }

    // Создаём MainLoop и сохраняем
    Pipeline *ml = build_main_loop_pipeline();
    if (ml) {
        algorithm_save(txn, main_loop_algo_id, ml);
        free(ml->code);
        free(ml);
    }
}

void* dmn_loop(void* arg) {
    (void)arg;
    while(dmn_running && g_running) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
        if (!dmn_running || !g_running) break;

        MDB_txn *txn = NULL;
        if (mdb_txn_begin(db.env, NULL, 0, &txn) != MDB_SUCCESS) continue;
        if (!dmn_running) { mdb_txn_abort(txn); break; }

        ensure_main_loop_exists(txn);

        Pipeline *main_loop = NULL;
        if (algorithm_load(txn, main_loop_algo_id, &main_loop) == 0 && main_loop) {
            VMContext ctx;
            memset(&ctx, 0, sizeof(ctx));
            vm_init(&ctx, txn, global_wm);
            ctx.hyper_mem = global_hyper_mem;
            ctx.current_context = 0;
            ctx.current_episode_id = 0;

            int rc = vm_execute(&ctx, main_loop);
            if (rc != VM_OK) {
                LOG_ERROR("MainLoop execution failed with status %d", rc);
            }

            if (main_loop->code) free(main_loop->code);
            free(main_loop);
        }
        mdb_txn_commit(txn);
    }
    LOG_MEMORY("Subconscious daemon stopped.");
    return NULL;
}

void start_subconscious_daemon(WorkingMemory *wm) {
    if (dmn_running) return;
    global_wm = wm;
    dmn_running = 1;
    pthread_create(&dmn_thread, NULL, dmn_loop, NULL);
}

void stop_subconscious_daemon(void) {
    if (!dmn_running) return;
    LOG_MEMORY("Stopping subconscious daemon...");
    dmn_running = 0;
    pthread_join(dmn_thread, NULL);
    LOG_MEMORY("Subconscious daemon stopped.");
}
