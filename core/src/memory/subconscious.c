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
static WorkingMemory *global_wm = NULL;
static uint64_t main_loop_algo_id = 0;

/* -----------------------------------------------
 * Гипер-операторный MainLoop
 * ----------------------------------------------- */
static Pipeline* build_main_loop_pipeline(void) {
    Pipeline *p = calloc(1, sizeof(Pipeline));
    if (!p) return NULL;

    #define HASH_IS_A           0x17c4b3f0a95d8cfeULL   // djb2_hash("IS_A")
    #define HASH_GOAL           0x17c4b3f0a95d8d1aULL  // djb2_hash("Goal")
    #define HASH_HAS_ALGORITHM  0x4046433720830455175ULL  // djb2_hash("HAS_ALGORITHM")

    Instruction code[] = {
        /* 0: reg[1] = 0 (базовый контекст) */
        { .operator_id = OP_LOAD_CONST, .arg[0] = 1, .arg[1] = 0 },
        /* 1: reg[2] = IS_A */
        { .operator_id = OP_LOAD_CONST, .arg[0] = 2, .arg[1] = HASH_IS_A },
        /* 2: reg[3] = Goal */
        { .operator_id = OP_LOAD_CONST, .arg[0] = 3, .arg[1] = HASH_GOAL },
        /* 3: OP_QUERY: process=IS_A, participant=Goal, context=0 -> scratchpad[10] */
        { .operator_id = OP_QUERY, .arg[0] = 2, .arg[1] = 3, .arg[2] = 1, .arg[3] = 10, .arg[4] = 7 },
        /* 4: если reg[7] == 0 → HALT (индекс 14) */
        { .operator_id = OP_BRANCH_IF_EMPTY, .arg[0] = 7, .arg[1] = 14 },
        /* 5: читаем первый goal id в reg[3] */
        { .operator_id = OP_READ_SP, .arg[0] = 3, .arg[1] = 10 },
        /* 6: reg[4] = HAS_ALGORITHM */
        { .operator_id = OP_LOAD_CONST, .arg[0] = 4, .arg[1] = HASH_HAS_ALGORITHM },
        /* 7: OP_QUERY: process=HAS_ALGORITHM, participant=goal, context=0 -> scratchpad[20] */
        { .operator_id = OP_QUERY, .arg[0] = 4, .arg[1] = 3, .arg[2] = 1, .arg[3] = 20, .arg[4] = 8 },
        /* 8: если reg[8] == 0 → HALT */
        { .operator_id = OP_BRANCH_IF_EMPTY, .arg[0] = 8, .arg[1] = 14 },
        /* 9: читаем первый algo id в reg[5] */
        { .operator_id = OP_READ_SP, .arg[0] = 5, .arg[1] = 20 },
        /* 10: spawn context → reg[6] */
        { .operator_id = OP_SPAWN_CTX, .arg[0] = 6 },
        /* 11: exec algorithm */
        { .operator_id = OP_EXEC_ALGORITHM, .arg[0] = 5 },
        /* 12: порог 0.8 в reg[10] */
        { .operator_id = OP_LOAD_CONST, .arg[0] = 10, .arg[1] = 0x3f4ccccd },
        /* 13: merge ctx */
        { .operator_id = OP_MERGE_CTX, .arg[0] = 10 },
        /* 14: HALT */
        { .operator_id = OP_HALT }
    };

    size_t num = sizeof(code) / sizeof(code[0]);
    p->code_len = (uint32_t)num;
    p->capacity = (uint32_t)num;
    p->code = malloc(sizeof(code));
    if (!p->code) { free(p); return NULL; }
    memcpy(p->code, code, sizeof(code));
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
            free(existing);  // только обёртка Pipeline
        }
        return;
    }

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
            vm_init(&ctx, txn, global_wm);
            ctx.hyper_mem = global_hyper_mem;
            ctx.current_context = 0;
            ctx.current_episode_id = 0;

            int rc = vm_execute(&ctx, main_loop);
            if (rc != VM_OK) {
                LOG_DEBUG("MainLoop execution failed with status %d", rc);
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
