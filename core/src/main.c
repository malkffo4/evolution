// core/src/main.c
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>    // open
#include <sys/file.h> // flock
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>

#include "memory/working.h"
#include "memory/subconscious.h"
#include "storage/db/db.h"
#include "storage/db/db_writer.h"
#include "ipc/ipc.h"
#include "core/globals.h"
#include "core/message_bus.h"
#include "runtime/logging/logging.h"
#include "runtime/operator/operator.h"
#include "execution/executor.h"


#define VERSION "0.4.0"

#define LOCKFILE "/tmp/evolution.lock"

static int lock_fd = -1;

// Обработчик сигналов прерывания
static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
    bus_stop(); // Переводим шины в статус остановки и будим спящие потоки
}

// Возвращает 0 при успехе, -1 при ошибке (блокировка уже занята)
int acquire_lock(void) {
    // Создаём файл блокировки, если его нет
    lock_fd = open(LOCKFILE, O_CREAT | O_RDWR, 0666);
    if (lock_fd == -1) {
        fprintf(stderr, "Cannot create lock file %s: %s\n", LOCKFILE, strerror(errno));
        return -1;
    }
    // Пытаемся захватить эксклюзивную блокировку (неблокирующую)
    if (flock(lock_fd, LOCK_EX | LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr, "Another instance is already running. Exiting.\n");
        } else {
            fprintf(stderr, "flock failed: %s\n", strerror(errno));
        }
        close(lock_fd);
        lock_fd = -1;
        return -1;
    }
    // Блокировка успешно захвачена
    return 0;
}

void release_lock(void) {
    if (lock_fd != -1) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        lock_fd = -1;
    }
}

// Полная и безопасная остановка всех систем ядра
static void shutdown_everything(void) {
    LOG_INFO("Graceful shutdown initiated...");

    // 1. Останавливаем подсознание (чтобы не дергало транзакции LMDB)
    stop_subconscious_daemon();

    // 2. Останавливаем фоновый экзекьютор скриптов
    executor_stop_daemon();
    bus_stop();
    // 3. Выключаем IPC сервер (закрывает сокеты и завершает потоки клиентов)
    ipc_shutdown();
    bus_wakeup_all();
    // 4. Очищаем оперативную рабочую память
    wm_clear(&global_wm);

    // 5. Закрываем базу данных и файлы логов
    db_writer_stop();
    close_lmdb();
    log_shutdown();
    release_lock();
}

// Инициализация всех систем ядра
static int init_everything(const char *db_path) {
    // Регистрируем обработчики сигналов для мягкого выхода
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Игнорируем SIGPIPE, чтобы ядро не падало при внезапном отключении клиентов
    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa_pipe, NULL);

    if (log_init("logs") != 0) {
        fprintf(stderr, "Failed to initialize logging.\n");
        return -1;
    }

    if (acquire_lock())
        return -1;

    LOG_INFO("Evolution Core %s starting...", VERSION);

    // Инициализация рабочей памяти (Working Memory)
    if (wm_init(&global_wm, 256) != 0) {
        LOG_ERROR("Failed to initialize Working Memory");
        return EXIT_FAILURE;
    }

    if (init_lmdb(db_path) != MDB_SUCCESS) {
        LOG_ERROR("Cannot initialize database.");
        return -1;
    }

    if (db_writer_start() != 0) {
        LOG_ERROR("Failed to start DB writer thread.");
        return -1;
    }

    // Инициализируем HyperMemory
    MDB_txn *txn;
    if (mdb_txn_begin(db.env, NULL, 0, &txn) == MDB_SUCCESS) {
        global_hyper_mem = hyper_memory_new(db.graph.hyper.atoms,
            db.graph.hyper.idx_process,
            db.graph.hyper.idx_args,
            db.graph.hyper.idx_context);
        mdb_txn_commit(txn);
    } else {
        LOG_ERROR("Failed to begin transaction for HyperMemory init");
        return -1;
    }
    hyper_memory_set_db_causal(global_hyper_mem, db.graph.hyper.idx_causal);
    hyper_memory_set_db_archive(global_hyper_mem, db.graph.hyper.archive);
    hyper_memory_set_db_vectors(global_hyper_mem, db.graph.hyper.idx_vectors);
    hyper_memory_set_db_pending(global_hyper_mem, db.graph.hyper.idx_pending);

    // Реестр операторов виртуальной машины
    operator_registry_init();

    if (ipc_init() != IPC_OK) {
        LOG_ERROR("IPC initialization failed.");
        return -1;
    }

    // Запуск фонового демона подсознания (когнитивные процессы)
    start_subconscious_daemon();

    // Запуск демона выполнения внешних скриптов
    executor_start_daemon();

    return 0;
}

int main(int argc, char *argv[]) {
    const char *db_path = "./data";

    static struct option long_options[] = {
        {"db-path", required_argument, 0, 'd'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                db_path = optarg;
                break;
            case 'h':
                printf("Usage: evolution_core [--db-path <path>]\n");
                return 0;
            default:
                fprintf(stderr, "Unknown option. Use --help\n");
                return 1;
            }
        }

    if (init_everything(db_path) != 0) {
        shutdown_everything();
        return EXIT_FAILURE;
    }

    LOG_INFO("System ready, waiting for IPC messages...");

    // ИСПРАВЛЕНИЕ: Главный поток просто ждет сигнала завершения (g_running = 0)
    // Всю работу по приему и диспетчеризации делают потоки клиентов
    while (g_running) { //[cite: 32]
        sleep(1); // Засыпаем, чтобы не перегружать CPU
    }

    // Полное высвобождение ресурсов
    shutdown_everything();
    printf("[OK] Evolution Core stopped cleanly.\n");

    return EXIT_SUCCESS;
}
