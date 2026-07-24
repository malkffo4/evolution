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

void executor_start_daemon(void);
void executor_stop_daemon(void);

// === ДОБАВЛЕНЫ АСИНХРОННЫЕ ИНТЕРФЕЙСЫ ===
int executor_enqueue_script(const char *interpreter, const char *script_path,
                            char *const argv[], int *out_id);

int executor_get_result(int id, char **out_output,
                        int *out_exit_code, int *out_signal);
// =======================================

int executor_run_script_sync(const char *interpreter, const char *script_path,
                                    char *const argv[], char **out_output,
                                    int *out_exit_code, int *out_signal);

#endif // EXECUTOR_H
