#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>

#define EXPORT __attribute__((visibility("default")))

const char* module_name(void) {
    return "executor";
}

const char* module_description(void) {
    return "Python script executor with venv support";
}

const char** module_function_names(void) {
    static const char *names[] = {
        "executor_start_daemon",
        "executor_stop_daemon",
        "executor_enqueue_script",
        "executor_get_result",
        NULL
    };
    return names;
}

typedef struct ExecTask {
    int id;
    unsigned timeout_ms;
    volatile int cancelled;
    pid_t child_pid;
    char *interpreter;
    char *script_path;
    char **argv;
    struct ExecTask *next;
} ExecTask;

typedef struct ExecResult {
    int id;
    char *output;
    int exit_code;
    int term_signal;
    struct ExecResult *next;
} ExecResult;

static pthread_t exec_thread;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
static ExecTask *queue_head = NULL;
static ExecTask *queue_tail = NULL;
static ExecResult *results_head = NULL;
static int executor_running = 0;
static int next_task_id = 1;

// Освобождение задачи
static void free_task(ExecTask *t) {
    if (!t) return;
    free(t->interpreter);
    free(t->script_path);
    if (t->argv) {
        for (int i = 0; t->argv[i]; i++) free(t->argv[i]);
        free(t->argv);
    }
    free(t);
}

// Освобождение результата
static void free_result(ExecResult *r) {
    if (!r) return;
    free(r->output);
    free(r);
}

// Очистка всей очереди результатов (при остановке демона)
static void free_all_results(void) {
    ExecResult *curr = results_head;
    while (curr) {
        ExecResult *next = curr->next;
        free_result(curr);
        curr = next;
    }
    results_head = NULL;
}

static void push_task(ExecTask *t) {
    if (!t) return;
    if (!queue_tail) queue_head = queue_tail = t;
    else { queue_tail->next = t; queue_tail = t; }
}

static ExecTask *pop_task(void) {
    ExecTask *t = queue_head;
    if (!t) return NULL;
    queue_head = t->next;
    if (!queue_head) queue_tail = NULL;
    t->next = NULL;
    return t;
}

static void push_result(ExecResult *r) {
    if (!r) return;
    r->next = results_head;
    results_head = r;
}

static char *read_all_from_fd(int fd) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    ssize_t n;
    while ((n = read(fd, buf + len, cap - len)) > 0) {
        len += (size_t)n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
    }
    if (n == -1 && errno != EINTR) {
        size_t extra = 128;
        char *nb = realloc(buf, len + extra);
        if (nb) {
            buf = nb;
            len += snprintf(buf + len, extra, "\n[read error: %s]", strerror(errno));
        }
    }
    if (buf) buf[len] = '\0';
    return buf;
}

static char **build_exec_argv(const char *interpreter, const char *script_path, char *const argv[]) {
    int extra = 2;
    int user_args = 0;
    if (argv) {
        while (argv[user_args]) user_args++;
    }
    char **a = calloc(extra + user_args + 1, sizeof(char*));
    if (!a) return NULL;
    a[0] = strdup(interpreter);
    a[1] = strdup(script_path);
    for (int i = 0; i < user_args; ++i) a[2 + i] = strdup(argv[i]);
    a[2 + user_args] = NULL;
    return a;
}

static void free_argv_array(char **a) {
    if (!a) return;
    for (int i = 0; a[i]; ++i) free(a[i]);
    free(a);
}

static void *executor_thread_main(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&queue_mutex);
        while (executor_running && !queue_head)
            pthread_cond_wait(&queue_cond, &queue_mutex);
        if (!executor_running && !queue_head) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        ExecTask *task = pop_task();
        pthread_mutex_unlock(&queue_mutex);
        if (!task) continue;

        int pipefd[2];
        if (pipe(pipefd) == -1) {
            ExecResult *res = calloc(1, sizeof(*res));
            res->id = task->id;
            res->output = strdup("[ERROR] failed to create pipe");
            res->exit_code = -1;
            push_result(res);
            free_task(task);
            continue;
        }

        pid_t pid = fork();
        if (pid == -1) {
            close(pipefd[0]); close(pipefd[1]);
            ExecResult *res = calloc(1, sizeof(*res));
            res->id = task->id;
            res->output = strdup("[ERROR] fork failed");
            res->exit_code = -1;
            push_result(res);
            free_task(task);
            continue;
        }

        if (pid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]); close(pipefd[1]);

            char **exec_argv = build_exec_argv(task->interpreter, task->script_path, task->argv);
            if (!exec_argv) {
                dprintf(STDERR_FILENO, "[CHILD ERROR] failed to build argv\n");
                _exit(127);
            }
            execvp(exec_argv[0], exec_argv);
            dprintf(STDERR_FILENO, "[CHILD ERROR] exec failed: %s\n", strerror(errno));
            free_argv_array(exec_argv);
            _exit(127);
        } else {
            close(pipefd[1]);
            char *out = read_all_from_fd(pipefd[0]);
            close(pipefd[0]);

            int status = 0;
            pid_t w = waitpid(pid, &status, 0);
            ExecResult *res = calloc(1, sizeof(*res));
            res->id = task->id;
            res->output = out ? out : strdup("");
            if (w == -1) {
                res->exit_code = -1;
            } else {
                if (WIFEXITED(status)) res->exit_code = WEXITSTATUS(status);
                else if (WIFSIGNALED(status)) res->term_signal = WTERMSIG(status);
            }
            pthread_mutex_lock(&queue_mutex);
            push_result(res);
            pthread_mutex_unlock(&queue_mutex);
            free_task(task);
        }
    }
    return NULL;
}
// int EXPORT
int executor_start_daemon(void) {
    pthread_mutex_lock(&queue_mutex);
    if (executor_running) { pthread_mutex_unlock(&queue_mutex); return 0; }
    executor_running = 1;
    int rc = pthread_create(&exec_thread, NULL, executor_thread_main, NULL);
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
    return rc;
}
// int EXPORT
int executor_stop_daemon(void) {
    pthread_mutex_lock(&queue_mutex);
    if (!executor_running) { pthread_mutex_unlock(&queue_mutex); return 0; }
    executor_running = 0;
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
    pthread_join(exec_thread, NULL);

    // Очистить все невостребованные результаты
    free_all_results();
    return 0;
}

int EXPORT executor_enqueue_script(const char *interpreter, const char *script_path,
                                   char *const argv[], int *out_id) {
    if (!interpreter || !script_path) return -1;
    ExecTask *t = calloc(1, sizeof(*t));
    if (!t) return -1;
    t->interpreter = strdup(interpreter);
    t->script_path = strdup(script_path);
    if (argv) {
        int cnt = 0; while (argv[cnt]) cnt++;
        t->argv = calloc(cnt + 1, sizeof(char*));
        for (int i = 0; i < cnt; i++) t->argv[i] = strdup(argv[i]);
        t->argv[cnt] = NULL;
    }
    pthread_mutex_lock(&queue_mutex);
    t->id = next_task_id++;
    push_task(t);
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
    if (out_id) *out_id = t->id;
    return 0;
}

int EXPORT executor_get_result(int id, char **out_output, int *out_exit_code, int *out_signal) {
    if (!id) return -1;
    pthread_mutex_lock(&queue_mutex);
    ExecResult **p = &results_head;
    while (*p) {
        if ((*p)->id == id) break;
        p = &((*p)->next);
    }
    if (!*p) {
        pthread_mutex_unlock(&queue_mutex);
        return 1;
    }
    ExecResult *found = *p;
    *p = found->next;
    pthread_mutex_unlock(&queue_mutex);

    if (out_output) *out_output = found->output;
    else free(found->output);
    if (out_exit_code) *out_exit_code = found->exit_code;
    if (out_signal) *out_signal = found->term_signal;
    free(found);
    return 0;
}

int EXPORT executor_run_script_sync(const char *interpreter, const char *script_path,
                                    char *const argv[], char **out_output,
                                    int *out_exit_code, int *out_signal) {
    int id = 0;
    if (executor_enqueue_script(interpreter, script_path, argv, &id) != 0) return -1;
    for (;;) {
        char *out = NULL; int ec = 0, sig = 0;
        int r = executor_get_result(id, &out, &ec, &sig);
        if (r == 0) {
            if (out_output) *out_output = out; else free(out);
            if (out_exit_code) *out_exit_code = ec;
            if (out_signal) *out_signal = sig;
            return 0;
        }
        usleep(10000);
    }
    return -1;
}
