#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "execution/executor.h"
#include "runtime/logging/logging.h"

static volatile int exec_running = 0;
static pthread_t exec_thread;

// Простой воркер для фоновых задач (пока заглушка для демона)
static void* executor_worker(void* arg) {
    (void)arg;
    LOG_INFO("Executor daemon started. Ready to run system tasks.");
    while (exec_running) {
        // Здесь в будущем будет выборка задач из очереди (MDB или Message Bus)
        usleep(100000); // 100ms
    }
    LOG_INFO("Executor daemon stopped.");
    return NULL;
}

void executor_start_daemon(void) {
    if (exec_running) return;
    exec_running = 1;
    if (pthread_create(&exec_thread, NULL, executor_worker, NULL) != 0) {
        LOG_ERROR("Failed to start executor thread");
        exec_running = 0;
    }
}

void executor_stop_daemon(void) {
    if (!exec_running) return;
        exec_running = 0;
    pthread_join(exec_thread, NULL);
}

// Безопасный запуск команды и чтение её вывода (STDOUT + STDERR)
ExecStatus executor_run_sync(const char *cmd, char *out_buffer, uint32_t max_len, int *exit_code) {
    if (!cmd || !out_buffer || max_len == 0) return EXEC_ERR_INVALID;

    memset(out_buffer, 0, max_len);
    LOG_DEBUG("Executing command: %s", cmd);

    // Добавляем 2>&1 чтобы перехватить и ошибки тоже
    char full_cmd[MAX_CMD_LENGTH + 10];
    snprintf(full_cmd, sizeof(full_cmd), "%s 2>&1", cmd);

    FILE *fp = popen(full_cmd, "r");
    if (fp == NULL) {
        LOG_ERROR("Failed to run command: %s", cmd);
        return EXEC_ERR_EXEC;
    }

    size_t total_read = 0;
    char chunk[1024];

    // Читаем вывод по кусочкам
    while (fgets(chunk, sizeof(chunk), fp) != NULL) {
        size_t len = strlen(chunk);
        if (total_read + len < max_len - 1) {
            strcat(out_buffer, chunk);
            total_read += len;
        } else {
            // Буфер переполнен, ставим маркер усечения
            strcat(out_buffer + total_read - 4, "...\n");
            break;
        }
    }

    int status = pclose(fp);
    if (exit_code) {
        // Извлекаем код возврата утилиты (например 0 - успех, 1 - ошибка)
        *exit_code = WEXITSTATUS(status);
    }

    LOG_DEBUG("Command execution finished. Exit code: %d, Output bytes: %zu", WEXITSTATUS(status), total_read);
    return EXEC_OK;
}
