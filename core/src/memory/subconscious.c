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
#include "critic_state.h"
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

static int dmn_running = 0;
static pthread_t dmn_thread;
static uint64_t main_loop_algo_id = 0;

/* -----------------------------------------------
 * Очередь задач
 * ----------------------------------------------- */
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

static Pipeline* build_critic_main_pipeline(void) {
    Pipeline *p = pipeline_create();

    Instruction code[] = {
        // R7 = сколько failures прочитано, scratchpad[0..] = (algo_id, count) пары
        { .operator_id = OP_READ_FAILURES, .arg = {0, 7} },
        // Дальше — цикл по failures: если count > 3, OP_ASSERT HAS_FLAW(algo_id)
        // и OP_DERIVE CONFIDENCE_DELTA(algo_id, -0.2)
        // (цикл на текущем ISA придётся развернуть через OP_JGE/OP_JMP —
        //  ровно то, что у вас уже заложено в opcode.h)
        { .operator_id = OP_HALT }
    };

    size_t num = sizeof(code)/sizeof(code[0]);
    p->code_len = (uint32_t)num;
    memcpy(p->code, code, sizeof(code));
    p->capacity = (uint32_t)num;
    return p;
}

/* -----------------------------------------------
 * Гипер-операторный MainLoop
 * ----------------------------------------------- */
static Pipeline* build_main_loop_pipeline(void) {
    Pipeline *p = pipeline_create();
    if (!p) return NULL;
    uint32_t critic_main_algo_id = 0; // TODO LOAD
    Instruction code[] = {
        { .operator_id = OP_LOAD_CONTEXT },
        { .operator_id = OP_SPREAD_ACTIVATION },
        { .operator_id = OP_EVALUATE_GOALS },
        { .operator_id = OP_CALL, .arg[0] = critic_main_algo_id },
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

// TODO в dmn_loop — параллельно с VM-циклом — читать get_pending_tasks() и слать executor_enqueue_script("python3", "app/knowledge/retrieval.py", argv_with_query, &task_id).
/* -----------------------------------------------
 * Основной цикл демона
 * ----------------------------------------------- */
void* dmn_loop(void* arg) {
    (void)arg;
    int rc;

    while(dmn_running && g_running) {
        // Уступаем процессор, если нет явного триггера
        if (!g_think_trigger) {
            struct timespec ts = {0, 100000000}; // 100 мс
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

        // Проверка карантина ПЕРЕД выполнением
        if (is_quarantined(main_loop_algo_id)) {
            mdb_txn_abort(txn);
            // Если MainLoop в карантине, спим дольше, чтобы дать системе остыть
            struct timespec ts = {1, 0};
            nanosleep(&ts, NULL);
            continue;
        }

        Pipeline *main_loop = NULL;
        if (algorithm_load(txn, main_loop_algo_id, &main_loop) == 0 && main_loop) {
            VMContext ctx;
            memset(&ctx, 0, sizeof(ctx));

            rc = vm_init(&ctx, txn, &global_wm);
            if (rc == VM_OK) {
                ctx.hyper_mem = global_hyper_mem;
                ctx.current_context = 0;
                ctx.current_episode_id = 0;

                rc = vm_execute(&ctx, main_loop);

                // Делегируем анализ успеха/провала Критику
                record_execution_result(main_loop_algo_id, rc);

                if (rc != VM_OK && rc != VM_NOT_FOUND) {
                    // TODO: Здесь можно добавить генерацию fail_atom в БД,
                    // когда структура HyperAtom будет полностью утверждена.
                    LOG_DEBUG("MainLoop execution halted with status %d", rc);
                }

                vm_destroy(&ctx);
            } else {
                LOG_ERROR("Error vm_init()");
            }
            pipeline_free(main_loop);
        }

        // Коммитим транзакцию, чтобы сохранить возможные полезные изменения
        // рабочей памяти до момента таймаута
        mdb_txn_commit(txn);
    }

    LOG_MEMORY("Subconscious daemon stopped.");
    return NULL;
}

void start_subconscious_daemon() {
    if (dmn_running) return;
    dmn_running = 1;
    init_quarantine();
    pthread_create(&dmn_thread, NULL, dmn_loop, NULL);
}

void stop_subconscious_daemon(void) {
    if (!dmn_running) return;
    LOG_MEMORY("Stopping subconscious daemon...");
    dmn_running = 0;
    pthread_join(dmn_thread, NULL);
    LOG_MEMORY("Subconscious daemon stopped.");
}
