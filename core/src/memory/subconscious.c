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
#include "reasoning/algorithm_planner.h"
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

typedef struct {
    bool executed_ok;   // MainLoop реально отработал и вернул VM_OK в этом тике?
    bool quarantined;   // MainLoop был в карантине — не исполнялся вовсе?
} MainLoopTickResult;

static int main_loop_tick_txn_fn(MDB_txn *txn, void *arg);

int subconscious_force_tick(void) {
    MainLoopTickResult tick_result = {0};
    int rc = db_write_sync(main_loop_tick_txn_fn, &tick_result);
    // Если транзакция упала — возвращаем ошибку
    if (rc != 0) return -1;
    // Если всё ок, возвращаем 1, если MainLoop реально отработал (VM_OK)
    return tick_result.executed_ok ? 1 : 0;
}

static int decay_txn_fn(MDB_txn *txn, void *arg) {
    (void)arg;
    if (!global_hyper_mem) return -1;
    hyper_memory_set_txn(global_hyper_mem, txn);

    MDB_stat stat;
    uint64_t total_atoms = 0;
    if (mdb_stat(txn, db.graph.hyper.atoms, &stat) == MDB_SUCCESS)
        total_atoms = (uint64_t)stat.ms_entries;

    homeostasis_step(&g_homeostasis, &HOMEOSTASIS_DEFAULT, &global_wm, total_atoms);

    DecayStats stats;
    return subconscious_decay_cycle(global_hyper_mem, &g_homeostasis.policy, &stats);
}

static void *decay_timer_loop(void *arg) {
    (void)arg;
    while (decay_timer_running && g_running) {
        struct timespec ts = {10, 0}; // раз в 10 секунд, независимо от MainLoop
        nanosleep(&ts, NULL);
        if (!decay_timer_running || !g_running) break;

        static int induction_nudge_counter = 0;
        if (++induction_nudge_counter >= 6) {   // ~раз в минуту при tick=10с
            induction_nudge_counter = 0;
            wm_activate(&global_wm, djb2_hash("InductiveSynthesisGoal"), 0.8f, 0.7f);
            for (uint32_t i = 0; i < global_wm.count; i++)
                if (global_wm.nodes[i].node_id == djb2_hash("InductiveSynthesisGoal"))
                    global_wm.nodes[i].state.usefulness = 0.85f;
        }

        int rc = db_write_sync(decay_txn_fn, NULL);
        if (rc != 0) LOG_WARN("[SUBCONSCIOUS] Timed decay cycle failed rc=%d", rc);
    }
    return NULL;
}

static void start_decay_timer(void) {
    decay_timer_running = 1;
    pthread_create(&decay_timer_thread, NULL, decay_timer_loop, NULL);
}

static void stop_decay_timer(void) {
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

static int main_loop_tick_txn_fn(MDB_txn *txn, void *arg) {
    MainLoopTickResult *result = arg;
    result->executed_ok = false;
    result->quarantined = false;

    if (global_hyper_mem)
        hyper_memory_set_txn(global_hyper_mem, txn);

    // Ядро просто ищет алгоритм в базе. Если Питоном он не залит — ядро спит.
    main_loop_algo_id = djb2_hash("MainLoop");

    if (is_quarantined(main_loop_algo_id)) {
        result->quarantined = true;
        return 0;
    }

    Pipeline *main_loop = NULL;
    // Если скрипт бутстрапа (Питон) залил MainLoop в LMDB — выполняем.
    if (algorithm_load(txn, main_loop_algo_id, &main_loop) == 0 && main_loop) {
        VMContext ctx;
        memset(&ctx, 0, sizeof(ctx));

        int rc = vm_init(&ctx, txn, &global_wm);
        if (rc == VM_OK) {
            ctx.hyper_mem = global_hyper_mem;
            ctx.current_context = 0;
            ctx.current_episode_id = 0;

            rc = vm_execute(&ctx, main_loop);

            record_execution_result(main_loop_algo_id, rc);

            if (rc == VM_OK) {
                result->executed_ok = true;
            } else if (rc != VM_NOT_FOUND) {
                LOG_DEBUG("MainLoop execution halted with status %d", rc);
            }
            vm_destroy(&ctx);
        }
        pipeline_free(main_loop);
    }

    return 0;
}

/* -----------------------------------------------
 * Основной цикл демона
 * ----------------------------------------------- */
void* dmn_loop(void* arg) {
    (void)arg;
    int idle_cycles = 0;

    while (dmn_running && g_running) {
        // Уступаем процессор, если нет явного триггера
        if (!g_think_trigger) {
            int delay_ms = 100 * (1 << (idle_cycles > 5 ? 5 : idle_cycles));
            struct timespec ts = {delay_ms / 1000, (delay_ms % 1000) * 1000000};
            nanosleep(&ts, NULL);
        } else {
            g_think_trigger = 0;
        }

        if (!dmn_running || !g_running) break;

        MainLoopTickResult tick_result = {0};
        int rc = db_write_sync(main_loop_tick_txn_fn, &tick_result);

        if (rc != 0) {
            // Инфраструктурная ошибка db_writer (остановлен/очередь полна/
            // commit не удался) — не паника, штатное while-условие само
            // корректно завершит цикл при shutdown на следующей итерации.
            LOG_WARN("[SUBCONSCIOUS] MainLoop tick: db_write_sync failed rc=%d", rc);
            continue;
        }

        if (tick_result.quarantined) {
            // Сон держит ИМЕННО поток dmn_loop — писатель уже свободен,
            // транзакция закоммичена и завершена внутри db_write_sync()
            // до возврата сюда.
            struct timespec ts = {1, 0};
            nanosleep(&ts, NULL);
            idle_cycles++;
            continue;
        }

        if (tick_result.executed_ok)
            idle_cycles = 0;
        else
            idle_cycles++;
    }

    LOG_MEMORY("Subconscious daemon stopped.");
    return NULL;
}

void start_subconscious_daemon() {
    if (dmn_running) return;
    dmn_running = 1;
    init_quarantine();
    start_decay_timer();
    pthread_create(&dmn_thread, NULL, dmn_loop, NULL);
}

void stop_subconscious_daemon(void) {
    if (!dmn_running) return;
    LOG_MEMORY("Stopping subconscious daemon...");
    dmn_running = 0;
    stop_decay_timer();
    pthread_join(dmn_thread, NULL);
    LOG_MEMORY("Subconscious daemon stopped.");
}
