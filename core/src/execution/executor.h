// execution/executor.h
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <stdint.h>
#include "types/exec.h"

#define MAX_CMD_LENGTH 1024
#define MAX_OUTPUT_LENGTH 65536 // 64KB для вывода команд (nmap и т.д.)

// Структура задачи на выполнение
typedef struct {
    uint64_t task_id;
    char command[MAX_CMD_LENGTH];
    char output[MAX_OUTPUT_LENGTH];
    int exit_code;
    ExecStatus status;
} ExecTask;

// Инициализация и запуск демона (вызывается в main.c)
void executor_start_daemon(void);

// Остановка демона
void executor_stop_daemon(void);

// Синхронное выполнение команды с возвратом вывода (для IPC)
ExecStatus executor_run_sync(const char *cmd, char *out_buffer, uint32_t max_len, int *exit_code);

#endif // EXECUTOR_H
