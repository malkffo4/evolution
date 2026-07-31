// execution/executor.h
#pragma once

#include <stdint.h>

int executor_start_daemon(void);
int executor_stop_daemon(void);


int executor_enqueue_script(const char *interpreter, const char *script_path, char *const argv[], int *out_id);

int executor_get_result(int id, char **out_output, int *out_exit_code, int *out_signal);

int executor_run_script_sync(const char *interpreter, const char *script_path, char *const argv[], char **out_output,
                                    int *out_exit_code, int *out_signal);
