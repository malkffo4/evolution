// core/tests/executor_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>

#include "execution/executor.h"

static int tests_passed = 0;
static int tests_failed = 0;
static int tests_skipped = 0;

#define TEST(name) do { \
    printf("[TEST] %s... ", name); fflush(stdout); \
} while(0)

#define PASS() do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define SKIP(msg) do { printf("SKIPPED (%s)\n", msg); tests_skipped++; } while(0)

// Вспомогательная функция: создать простой echo-скрипт во временном файле
static char* create_script(const char *content) {
    char tmpname[] = "/tmp/evo_test_XXXXXX";
    int fd = mkstemp(tmpname);
    if (fd < 0) return NULL;
    write(fd, content, strlen(content));
    close(fd);
    return strdup(tmpname);
}

int main(void) {
    // Запускаем демон один раз для всех тестов
    assert(executor_start_daemon() == 0);

    // ========== Тест 1: Успешное выполнение ==========
    TEST("simple_success");
    {
        char *script = create_script("#!/bin/sh\necho Hello");
        int id;
        assert(executor_enqueue_script("/bin/sh", script, NULL, &id) == 0);
        // Ждём результат
        char *out = NULL; int ec = 0, sig = 0;
        while (executor_get_result(id, &out, &ec, &sig) == 1) usleep(5000);
        assert(ec == 0);
        assert(sig == 0);
        assert(strstr(out, "Hello") != NULL);
        free(out); unlink(script); free(script);
        PASS();
    }

    // ========== Тест 2: Код возврата ==========
    TEST("exit_code");
    {
        char *script = create_script("#!/bin/sh\nexit 42");
        int id;
        executor_enqueue_script("/bin/sh", script, NULL, &id);
        char *out = NULL; int ec = 0, sig = 0;
        while (executor_get_result(id, &out, &ec, &sig) == 1) usleep(5000);
        assert(ec == 42);
        free(out); unlink(script); free(script);
        PASS();
    }

    // ========== Тест 3: Сигнал (убиваем процесс) ==========
    TEST("signal_kill");
    {
        // В реальном executor пока нет API для cancel (отмены задачи по ID).
        // Поэтому мы не ставим sleep в очередь, иначе executor_stop_daemon()
        // в конце будет ждать его 10 секунд. Честно скипаем тест.
        SKIP("needs cancel support in executor API");
    }

    // ========== Тест 4: Пустой вывод ==========
    TEST("empty_output");
    {
        char *script = create_script("#!/bin/sh\ntrue");
        int id;
        executor_enqueue_script("/bin/sh", script, NULL, &id);
        char *out = NULL;
        while (executor_get_result(id, &out, NULL, NULL) == 1) usleep(5000);
        assert(out && strlen(out) == 0); // true не печатает ничего
        free(out); unlink(script); free(script);
        PASS();
    }

    // ========== Тест 5: Большой вывод (100 КБ) ==========
    TEST("large_output");
    {
        char *script = create_script(
            "#!/bin/sh\n"
            "dd if=/dev/zero bs=100000 count=1 2>/dev/null | od -A n -v -t u1\n"
        );
        int id;
        executor_enqueue_script("/bin/sh", script, NULL, &id);
        char *out = NULL;
        while (executor_get_result(id, &out, NULL, NULL) == 1) usleep(5000);
        assert(out != NULL);
        size_t len = strlen(out);
        assert(len > 100000); // вывод od будет значительно больше бинарного блока
        free(out); unlink(script); free(script);
        PASS();
    }

    // ========== Тест 6: Параллельное выполнение ==========
    TEST("parallel_tasks");
    {
        #define PAR_COUNT 4
        int ids[PAR_COUNT];
        char *scripts[PAR_COUNT];
        for (int i = 0; i < PAR_COUNT; i++) {
            char buf[100];
            snprintf(buf, sizeof(buf), "#!/bin/sh\necho Task%d", i);
            scripts[i] = create_script(buf);
            executor_enqueue_script("/bin/sh", scripts[i], NULL, &ids[i]);
        }
        // собираем все результаты
        int completed = 0;
        while (completed < PAR_COUNT) {
            for (int i = 0; i < PAR_COUNT; i++) {
                if (ids[i] != 0) {
                    char *out = NULL; int ec;
                    if (executor_get_result(ids[i], &out, &ec, NULL) == 0) {
                        assert(ec == 0);
                        char expected[20]; snprintf(expected, sizeof(expected), "Task%d", i);
                        assert(strstr(out, expected) != NULL);
                        free(out);
                        ids[i] = 0;
                        completed++;
                    }
                }
            }
            usleep(10000);
        }
        for (int i = 0; i < PAR_COUNT; i++) { unlink(scripts[i]); free(scripts[i]); }
        PASS();
    }

    // ========== Тест 7: Несуществующий скрипт ==========
    TEST("nonexistent_script");
    {
        int id;
        executor_enqueue_script("/bin/sh", "/tmp/nonexistent_script", NULL, &id);
        char *out = NULL; int ec = 0;
        while (executor_get_result(id, &out, &ec, NULL) == 1) usleep(5000);
        assert(ec != 0); // должен быть ненулевой код возврата
        free(out);
        PASS();
    }

    // ========== Тест 8: NULL параметры ==========
    TEST("null_params");
    {
        // enqueue с NULL interpreter или script_path должно вернуть -1
        assert(executor_enqueue_script(NULL, "foo", NULL, NULL) == -1);
        assert(executor_enqueue_script("bar", NULL, NULL, NULL) == -1);
        // get_result с нулевым id должно вернуть -1
        assert(executor_get_result(0, NULL, NULL, NULL) == -1);
        PASS();
    }

    // ========== Тест 9: Синхронный вызов ==========
    TEST("sync_call");
    {
        char *script = create_script("#!/bin/sh\necho sync_ok");
        char *out = NULL; int ec = 0;
        int rc = executor_run_script_sync("/bin/sh", script, NULL, &out, &ec, NULL);
        assert(rc == 0);
        assert(ec == 0);
        assert(strstr(out, "sync_ok") != NULL);
        free(out); unlink(script); free(script);
        PASS();
    }

    // ========== Завершение ==========
    executor_stop_daemon();

    printf("\n=== Executor Test Results ===\n");
    printf("Passed: %d\n", tests_passed);
    if (tests_skipped > 0) printf("Skipped: %d\n", tests_skipped);
    printf("Failed: %d\n", tests_failed);
    return tests_failed ? 1 : 0;
}
