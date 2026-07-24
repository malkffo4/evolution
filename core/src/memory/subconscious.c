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
volatile int g_think_trigger = 0;

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
static uint64_t main_loop_algo_id = 0;

/* -----------------------------------------------
 * Гипер-операторный MainLoop
 * ----------------------------------------------- */
static Pipeline* build_main_loop_pipeline(void) {
    Pipeline *p = pipeline_create();
    if (!p) return NULL;

    Instruction code[] = {
        { .operator_id = OP_LOAD_CONTEXT },
        { .operator_id = OP_SPREAD_ACTIVATION },
        { .operator_id = OP_EVALUATE_GOALS },
        { .operator_id = OP_HALT }
    };

    size_t num = sizeof(code) / sizeof(code[0]);
    p->code_len = (uint32_t)num;
    memcpy(p->code, code, sizeof(code));
    p->capacity = (uint32_t)num;
    p->constants.int_consts = NULL;
    p->constants.int_count = 0;

    return p;
}

static void ensure_main_loop_exists(MDB_txn *txn) {
    main_loop_algo_id = djb2_hash("MainLoop");

    Pipeline *existing = NULL;
    if (algorithm_load(txn, main_loop_algo_id, &existing) == 0) {
        if (existing) {
            // НЕ освобождаем existing->code!
            pipeline_free(existing);
        }
        return;
    }

    Pipeline *ml = build_main_loop_pipeline();
    if (ml) {
        algorithm_save(txn, main_loop_algo_id, ml);
        pipeline_free(ml);
    }
}

void* dmn_loop(void* arg) {
    (void)arg;
    int rc;

    while(dmn_running && g_running) {
        if (!g_think_trigger) {
            struct timespec ts = {0, 100000000};
            nanosleep(&ts, NULL);
        }
        g_think_trigger = 0;

        if (!dmn_running || !g_running) break;

        MDB_txn *txn = NULL;
        if (mdb_txn_begin(db.env, NULL, 0, &txn) != MDB_SUCCESS) continue;
        if (!dmn_running) { mdb_txn_abort(txn); break; }

        if (global_hyper_mem)
            hyper_memory_set_txn(global_hyper_mem, txn);

        ensure_main_loop_exists(txn);

        Pipeline *main_loop = NULL;
        if (algorithm_load(txn, main_loop_algo_id, &main_loop) == 0 && main_loop) {
            VMContext ctx;
            memset(&ctx, 0, sizeof(ctx));
            rc = vm_init(&ctx, txn, &global_wm);
            if (rc != VM_OK) {
                LOG_ERROR("Error vm_init()");
                continue;
            }
            ctx.hyper_mem = global_hyper_mem;
            ctx.current_context = 0;
            ctx.current_episode_id = 0;

            rc = vm_execute(&ctx, main_loop);
            if (rc != VM_OK && rc != VM_NOT_FOUND) {
                LOG_DEBUG("MainLoop execution failed with status %d", rc);
                // TODO CRITIC ERROR VM
                // struct TODO { int consecutive_failures, bool quarantined } node;
                // // 1. Классифицируй статус: EXPECTED (VM_NOT_FOUND — нет алгоритма, это нормально)
                // //    vs REAL_ERROR (VM_INVALID_REGISTER, VM_ERROR — баг в pipeline)
                // // 2. Дедупликация лога: не пиши одинаковую ошибку чаще раза в N секунд
                // // 3. Circuit breaker на уровне алгоритма/узла:
                // if (node.consecutive_failures++ > 3) {
                //     node.quarantined = true; // критик убирает узел из планирования
                //     analyze_error("repeated VM failure", node->node_id, txn); // теперь critic реально вызывается
                // }
                // Плюс у тебя уже ЕСТЬ статистика по операторам — ctx->profile[op->id].failures/calls в VMContext. Critic должен читать именно её, а не голый error_log (который сейчас вообще не парсится в analyze_error — только проверяется на strlen > 0). Доработка critic'а:

                // Принимать OperatorID/node_id_t algo_id + VMProfile вместо голой строки.
                // Не создавать один общий FAILURE_STATE на всё — разные типы отказов (VM_INVALID_REGISTER — баг компиляции pipeline; VM_TIMEOUT — зависание; VM_NOT_FOUND — нормальный "не знаю") должны давать разные узлы/рёбра.
                // Понижать confidence конкретного algorithm/edge, который выполнялся, а не абстрактного task_target_id.
                // Триггерить карантин при N подряд отказах — это твой автоматический "исправить/остановить".
                // OR ???
                // HyperAtom fail_atom = {
                //     .id = generate_id(ctx),  // нужен доступ к глобальному счётчику
                //     .process_id = djb2_hash("EXEC_FAILED"),
                //     .args = {
                //         HYPER_MAKE_REF(main_loop_algo_id),  // какой алгоритм упал
                //         (ko_id_t)rc | HYPER_TYPE_INT,       // код ошибки
                //         HYPER_MAKE_REF(goal_id)             // какая цель
                //     }
                // };
                // hyper_assert_unique(ctx.hyper_mem, &fail_atom);
            }
            vm_destroy(&ctx);
            pipeline_free(main_loop);
        }
        mdb_txn_commit(txn);
    }
    LOG_MEMORY("Subconscious daemon stopped.");
    return NULL;
}

void start_subconscious_daemon() {
    if (dmn_running) return;
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
