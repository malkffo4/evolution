// storage/db/db_writer.c
#include <stdlib.h>
#include <pthread.h>

#include "storage/db/db.h"
#include "storage/db/db_writer.h"
#include "runtime/logging/logging.h"

#define DB_WRITER_QUEUE_SIZE 256

typedef struct {
    DbWriteFn fn;
    void *arg;
    pthread_mutex_t *done_mutex;
    pthread_cond_t  *done_cond;
    int             *done_flag;
    int             *out_result;
} WriteJob;

static WriteJob queue[DB_WRITER_QUEUE_SIZE];
static int head = 0, tail = 0, count = 0;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  queue_cond  = PTHREAD_COND_INITIALIZER;
static pthread_t writer_thread;
static volatile int running = 0;

static void *writer_loop(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&queue_mutex);
        while (running && count == 0)
            pthread_cond_wait(&queue_cond, &queue_mutex);

        if (!running && count == 0) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }

        WriteJob job = queue[head];
        head = (head + 1) % DB_WRITER_QUEUE_SIZE;
        count--;
        pthread_mutex_unlock(&queue_mutex);

        MDB_txn *txn = NULL;
        int rc = mdb_txn_begin(db.env, NULL, 0, &txn);
        int result;

        if (rc != MDB_SUCCESS) {
            LOG_ERROR("db_writer: mdb_txn_begin failed: %s", mdb_strerror(rc));
            result = -1;
        } else {
            result = job.fn(txn, job.arg);
            if (result == 0) {
                rc = mdb_txn_commit(txn);
                if (rc != MDB_SUCCESS) {
                    LOG_ERROR("db_writer: commit failed: %s", mdb_strerror(rc));
                    result = -1;
                }
            } else {
                mdb_txn_abort(txn);
            }
        }

        if (job.done_flag) {
            pthread_mutex_lock(job.done_mutex);
            *job.done_flag = 1;
            if (job.out_result) *job.out_result = result;
            pthread_cond_signal(job.done_cond);
            pthread_mutex_unlock(job.done_mutex);
        }
    }
    return NULL;
}

int db_writer_start(void) {
    if (running) return 0;
    running = 1;
    if (pthread_create(&writer_thread, NULL, writer_loop, NULL) != 0) {
        running = 0;
        return -1;
    }
    LOG_INFO("DB writer thread started.");
    return 0;
}

void db_writer_stop(void) {
    if (!running) return;
    pthread_mutex_lock(&queue_mutex);
    running = 0;
    pthread_cond_broadcast(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
    pthread_join(writer_thread, NULL);
    LOG_INFO("DB writer thread stopped.");
}

static int enqueue(WriteJob *job) {
    pthread_mutex_lock(&queue_mutex);
    if (!running || count == DB_WRITER_QUEUE_SIZE) {
        pthread_mutex_unlock(&queue_mutex);
        return -1;
    }
    queue[tail] = *job;
    tail = (tail + 1) % DB_WRITER_QUEUE_SIZE;
    count++;
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
    return 0;
}

int db_write_async(DbWriteFn fn, void *arg) {
    WriteJob job = { .fn = fn, .arg = arg };
    return enqueue(&job);
}

int db_write_sync(DbWriteFn fn, void *arg) {
    pthread_mutex_t done_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  done_cond  = PTHREAD_COND_INITIALIZER;
    int done = 0, result = -1;

    WriteJob job = {
        .fn = fn, .arg = arg,
        .done_mutex = &done_mutex, .done_cond = &done_cond,
        .done_flag = &done, .out_result = &result
    };

    if (enqueue(&job) != 0) {
        pthread_mutex_destroy(&done_mutex);
        pthread_cond_destroy(&done_cond);
        return -1;
    }

    pthread_mutex_lock(&done_mutex);
    while (!done)
        pthread_cond_wait(&done_cond, &done_mutex);
    pthread_mutex_unlock(&done_mutex);

    pthread_mutex_destroy(&done_mutex);
    pthread_cond_destroy(&done_cond);
    return result;
}
