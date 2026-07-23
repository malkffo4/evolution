// runtime/vm/vm_pool.c
#include <string.h>
#include <pthread.h>
#include <stdlib.h>

#include "vm_pool.h"
#include "vm.h"

#define MAX_THREADS 4
static pthread_t threads[MAX_THREADS];
static int thread_count = 0;

void* vm_worker(void* arg) {
    Pipeline* p = (Pipeline*)arg;
    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    vm_init(&ctx, NULL, NULL);  // транзакция и WM будут переданы позже
    vm_execute(&ctx, p);
    free(p->code);
    free(p);
    return NULL;
}

void vm_pool_init(int max) {
    thread_count = (max > MAX_THREADS) ? MAX_THREADS : max;
}

void vm_pool_submit(Pipeline *pipeline, HyperMemory *hmem, WorkingMemory *wm) {
    for (int i = 0; i < thread_count; i++) {
        if (threads[i] == 0) {
            // грубо: создаём копию Pipeline
            Pipeline *copy = malloc(sizeof(Pipeline));
            *copy = *pipeline;
            copy->code = malloc(pipeline->code_len * sizeof(Instruction));
            memcpy(copy->code, pipeline->code, pipeline->code_len * sizeof(Instruction));
            pthread_create(&threads[i], NULL, vm_worker, copy);
            return;
        }
    }
    // если все потоки заняты – выполняем синхронно
    vm_worker(pipeline);
}

void vm_pool_shutdown(void) {
    for (int i = 0; i < thread_count; i++) {
        if (threads[i] != 0) pthread_join(threads[i], NULL);
    }
}
