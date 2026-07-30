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
#include "storage/db/db_writer.h"
#include "memory/working.h"
#include "memory/decay.h"
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

static pthread_t decay_timer_thread;
static volatile int decay_timer_running = 0;

static int decay_txn_fn(MDB_txn *txn, void *arg) {
    (void)arg;
    if (!global_hyper_mem) return -1;

    hyper_memory_set_txn(global_hyper_mem, txn);

    DecayStats stats;
    int rc = subconscious_decay_cycle(global_hyper_mem, &DECAY_POLICY_DEFAULT, &stats);
    return rc; // 0 -> commit, !=0 -> abort (см. db_writer.h::DbWriteFn)
}

static void *decay_timer_loop(void *arg) {
    (void)arg;
    while (decay_timer_running && g_running) {
        struct timespec ts = {10, 0}; // раз в 10 секунд, независимо от MainLoop
        nanosleep(&ts, NULL);
        if (!decay_timer_running || !g_running) break;

        int rc = db_write_sync(decay_txn_fn, NULL);
        if (rc != 0) LOG_WARN("[SUBCONSCIOUS] Timed decay cycle failed rc=%d", rc);
    }
    return NULL;
}

void start_decay_timer(void) {
    decay_timer_running = 1;
    pthread_create(&decay_timer_thread, NULL, decay_timer_loop, NULL);
}

void stop_decay_timer(void) {
    decay_timer_running = 0;
    pthread_join(decay_timer_thread, NULL);
}


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
        { .operator_id = OP_CRITIC_APPLY },
        { .operator_id = OP_HALT }
    };

    size_t num = sizeof(code)/sizeof(code[0]);
    memcpy(p->code, code, sizeof(code));
    p->code_len = (uint32_t)num;
    p->capacity = (uint32_t)num;
    return p;
}

static uint64_t g_critic_main_algo_id = 0;

static void ensure_critic_main_exists(MDB_txn *txn) {
    g_critic_main_algo_id = djb2_hash("CriticMain");

    Pipeline *existing = NULL;
    if (algorithm_load(txn, g_critic_main_algo_id, &existing) == 0) {
        if (existing) pipeline_free(existing);
        return;
    }
    Pipeline *cm = build_critic_main_pipeline();
    if (cm) {
        if (algorithm_save(txn, g_critic_main_algo_id, cm) != MDB_SUCCESS)
            LOG_ERROR("Failed to save CriticMain algorithm");
        pipeline_free(cm);
    }
}

static Pipeline* build_main_loop_pipeline(void) {
    Pipeline *p = pipeline_create();
    if (!p) return NULL;

    Instruction code[] = {
        { .operator_id = OP_LOAD_CONTEXT },
        { .operator_id = OP_SPREAD_ACTIVATION },
        { .operator_id = OP_EVALUATE_GOALS },
        { .operator_id = OP_CALL, .arg[0] = (uint32_t)g_critic_main_algo_id },
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
        int rc = algorithm_save(txn, main_loop_algo_id, ml);
        if (rc != MDB_SUCCESS)
            LOG_ERROR("main_loop_algo_id not saved.");
        pipeline_free(ml);
    }
}

static Pipeline* build_core_planner_pipeline(void) {
    Pipeline *p = pipeline_create();
    if (!p) return NULL;

    // ИСПРАВЛЕНИЕ: раньше здесь был OP_LOAD_CONTEXT перед OP_HALT. Поскольку
    // vm_op_evaluate_goals() считает пайплайн "содержащим логику", если в
    // нём есть ЛЮБАЯ инструкция кроме OP_HALT, присутствие OP_LOAD_CONTEXT
    // ошибочно помечало пустую заглушку как "готовый планировщик" и НАВСЕГДА
    // прерывало выполнение до шага 2 (fallback wm_get_highest_goal +
    // planner_select_algorithm). Реальный Goal->Algorithm цикл никогда не
    // запускался через MainLoop/think. Оставляем заглушку буквально пустой,
    // как и написано в комментарии автора ниже.
    //
    // Пока CorePlanner — просто заглушка.
    // В будущем сюда будет добавлен байт-код для обратного вывода.
    Instruction code[] = {
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

// В ensure_main_loop_exists (или рядом) добавить:
static void ensure_core_planner_exists(MDB_txn *txn) {
    uint64_t core_planner_id = djb2_hash("CorePlanner");
    Pipeline *existing = NULL;
    if (algorithm_load(txn, core_planner_id, &existing) == 0) {
        if (existing) pipeline_free(existing);
        return;
    }
    Pipeline *cp = build_core_planner_pipeline();
    if (cp) {
        if (algorithm_save(txn, core_planner_id, cp) != MDB_SUCCESS)
            LOG_ERROR("Failed to save CorePlanner algorithm");
        pipeline_free(cp);
    }
}

// TODO в dmn_loop — параллельно с VM-циклом — читать get_pending_tasks() и слать
// executor_enqueue_script("python3", "app/knowledge/retrieval.py", argv_with_query, &task_id).
/* -----------------------------------------------
 * Основной цикл демона
 * ----------------------------------------------- */
void* dmn_loop(void* arg) {
    (void)arg;
    int rc;
    int idle_cycles = 0;

    while(dmn_running && g_running) {
        // Уступаем процессор, если нет явного триггера
        if (!g_think_trigger) {
            int delay_ms = 100 * (1 << (idle_cycles > 5 ? 5 : idle_cycles));
            struct timespec ts = {delay_ms / 1000, (delay_ms % 1000) * 1000000};
            nanosleep(&ts, NULL);
        } else {
            g_think_trigger = 0;
        }

        if (!dmn_running || !g_running) break;

        MDB_txn *txn = NULL;
        if (mdb_txn_begin(db.env, NULL, 0, &txn) != MDB_SUCCESS) continue;
        if (!dmn_running) { mdb_txn_abort(txn); break; }

        if (global_hyper_mem)
            hyper_memory_set_txn(global_hyper_mem, txn);

        ensure_critic_main_exists(txn);
        ensure_core_planner_exists(txn);
        ensure_main_loop_exists(txn);

        // Проверка карантина ПЕРЕД выполнением
        if (is_quarantined(main_loop_algo_id)) {
            mdb_txn_abort(txn);
            // Если MainLoop в карантине, спим дольше, чтобы дать системе остыть
            struct timespec ts = {1, 0};
            nanosleep(&ts, NULL);
            continue;
        }
        bool main_loop_executed_ok = false;
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

                if (rc == VM_OK) {
                    // TODO: Здесь можно добавить генерацию fail_atom в БД,
                    // когда структура HyperAtom будет полностью утверждена.
                    main_loop_executed_ok = true;   // успешное выполнение
                } else if (rc == VM_NOT_FOUND) {
                    // нет целей – цикл был холостым, не сбрасываем idle_cycles
                } else {
                    LOG_DEBUG("MainLoop execution halted with status %d", rc);
                }
                vm_destroy(&ctx);
            } else {
                LOG_ERROR("Error vm_init()");
            }
            pipeline_free(main_loop);
        }
        if (main_loop_executed_ok)   // введите признак "была полезная работа"
            idle_cycles = 0;
        else
            idle_cycles++;

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
