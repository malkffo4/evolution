// main.c
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include "memory/working.h"
#include "memory/subconscious.h"
#include "storage/db/db.h"
#include "ipc/ipc.h"
#include "core/message_bus.h"
#include "runtime/logging/logging.h"
#include "runtime/operator/operator.h"
#include "execution/executor.h"
#include "main.h"

#define VERSION "0.4.0"

volatile sig_atomic_t g_running = 1;
WorkingMemory global_wm;

static void signal_handler(int sig) {
    (void)sig;
    static int already = 0;
    if (already) _exit(0);
    already = 1;
    g_running = 0;
    LOG_INFO("Shutdown signal received, stopping...");
    stop_subconscious_daemon();
    bus_wakeup_all();
}

static void shutdown_everything(void) {
    LOG_INFO("Shutting down...");

    executor_stop_daemon();

    stop_subconscious_daemon();  // на всякий случай

    wm_clear(&global_wm);

    ipc_shutdown();

    close_lmdb();

    log_shutdown();
}

static int init_everything(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (log_init("logs") != 0) {
        LOG_ERROR("Failed to initialize logging.");
        return -1;
    }

    LOG_INFO("Evolution Core %s starting...", VERSION);

    if (init_lmdb("./data") != MDB_SUCCESS) {
        LOG_ERROR("Cannot initialize database.");
        return -1;
    }
    LOG_GRAPH("Graph database initialized.");

    // Инициализируем операторы VM
    operator_registry_init();
    LOG_DEBUG("Operator registry initialized.");

    if (ipc_init() != IPC_OK) {
        LOG_ERROR("IPC initialization failed.");
        return -1;
    }
    LOG_IPC("IPC initialized.");

    executor_start_daemon();

    return 0;
}

int main(void) {
    // Инициализация WM с размерами
    if (wm_init(&global_wm, 256, 512) != 0) {
        LOG_ERROR("Failed to initialize Working Memory");
        return EXIT_FAILURE;
    }

    if (init_everything() != 0) {
        return EXIT_FAILURE;
    }

    // Запускаем демон подсознания
    start_subconscious_daemon(&global_wm);

    LOG_INFO("System ready, waiting for IPC messages...");

    // Основной цикл с таймаутом для проверки флага
    while (g_running) {
        IPCPacket request, response;
        IPCStatus st = ipc_receive(&request);

        if (st == IPC_DISCONNECTED) {
            LOG_IPC("IPC disconnected.");
            break;
        }

        if (st != IPC_OK) {
            // Небольшая задержка чтобы не спамить CPU
            usleep(10000);
            continue;
        }

        ipc_dispatch(&request, &response);
    }

    shutdown_everything();
    LOG_INFO("Evolution Core stopped.");
    return EXIT_SUCCESS;
}
