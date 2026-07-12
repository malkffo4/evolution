// subconscious_daemon.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "reasoning/engine.h"
#include "storage/db/db.h"
#include "subconscious.h"
#include "storage/string_pool/string_pool.h"
#include "memory/working.h"

static ResearchTask task_queue[MAX_PENDING_TASKS];
static int task_count = 0;
static pthread_mutex_t task_mutex = PTHREAD_MUTEX_INITIALIZER;

// Добавление задачи в очередь (потокобезопасно)
void enqueue_research_task(uint64_t node_id, const char *query) {
    pthread_mutex_lock(&task_mutex);
    if (task_count < MAX_PENDING_TASKS) {
        task_queue[task_count].node_id = node_id;
        strncpy(task_queue[task_count].query, query, sizeof(task_queue[task_count].query) - 1);
        task_count++;
    }
    pthread_mutex_unlock(&task_mutex);
}

// Функция, которую вызовет Python-сервис (пока заглушка)
int get_pending_tasks(ResearchTask *buffer, int max_count) {
    int cnt = 0;
    pthread_mutex_lock(&task_mutex);
    cnt = (task_count < max_count) ? task_count : max_count;
    memcpy(buffer, task_queue, cnt * sizeof(ResearchTask));
    // Удаляем взятые задачи
    if (cnt > 0) {
        memmove(task_queue, task_queue + cnt, (task_count - cnt) * sizeof(ResearchTask));
        task_count -= cnt;
    }
    pthread_mutex_unlock(&task_mutex);
    return cnt;
}
/* ------------------------------------------------------------ */

static int dmn_running = 0;
static pthread_t dmn_thread;
static WorkingMemory *global_wm;

void* dmn_loop(void* arg) {
    (void)arg;

    while(dmn_running) {
        sleep(2); // Тик каждые 2 секунды

        MDB_txn *txn = NULL;
        if (mdb_txn_begin(db.env, NULL, 0, &txn) != MDB_SUCCESS) continue;

        // 1. Когнитивный цикл
        engine_spread_activation(global_wm, txn);
        // hypothesis_engine(global_wm, txn);
        wm_decay(global_wm);

        // 2. Поиск новых знаний (любопытство) → добавляем в очередь задач
        for (uint32_t i = 0; i < global_wm->count; i++) {
            WorkingNode *n = &global_wm->nodes[i];

            if (n->activation > 0.5f && n->state.novelty > 0.6f) {
                // Проверяем, нет ли уже такой задачи в очереди
                int already_queued = 0;
                pthread_mutex_lock(&task_mutex);
                for (int t = 0; t < task_count; t++) {
                    if (task_queue[t].node_id == n->node_id) {
                        already_queued = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&task_mutex);

                if (!already_queued) {
                    const char *word_name = get_string_from_pool(txn, n->node_id);
                    if (word_name) {
                        printf("\n\033[35m[ПОДСОЗНАНИЕ] Новое понятие '%s' → в очередь на исследование\033[0m\n", word_name);
                        enqueue_research_task(n->node_id, word_name);
                        // free(word_name);
                        n->state.novelty *= 0.5f; // Чтобы не спамить
                    }
                }
            }
        }

        mdb_txn_commit(txn);
    }
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
    dmn_running = 0;
    pthread_join(dmn_thread, NULL);
}
