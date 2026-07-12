#include <signal.h>
#include <stdlib.h>

#include "memory/working.h"
#include "memory/subconscious.h"
#include "storage/db/db.h"
#include "ipc/ipc.h"
#include "core/message_bus.h"
#include "runtime/logging/logging.h"

#define VERSION "0.4.0"

static volatile sig_atomic_t g_running = 1;
WorkingMemory global_wm;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
    bus_wakeup_all(); // Будим ipc_receive, чтобы выйти из цикла
}

static int init_everything(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    log_init("logs");

    LOG_INFO("Evolution Core %s starting...", VERSION);

    if (init_lmdb("./data") != MDB_SUCCESS) {
        LOG_ERROR("Cannot initialize database.");
        return -1;
    }

    LOG_GRAPH("Graph database initialized.");

    if (ipc_init() != IPC_OK) {
        LOG_ERROR("IPC initialization failed.");
        return -1;
    }

    LOG_IPC("IPC initialized.");

    return 0;
}

static void shutdown_everything(void)
{
    LOG_INFO("Shutting down...");

    ipc_shutdown();
    close_lmdb();

    log_shutdown();
}
// TODO
// Pipeline* load_pipeline_from_db(pipeline_id)

// void TODO(void) {
//     // Где-то при загрузке модели:
//     void *neural_policy_model = load_model("policy_v1.onnx");
//     operator_register_compiled(POLICY_NEURAL_V1, "neural_policy_v1", 0, neural_policy_model, NULL, 0, 0);

//     // Создаём обёртку PlannerPolicy, которая вызывает модель
//     static const Operator *neural_policy_choose(VMContext *ctx, CapabilityMask cap) {
//         // используем neural_policy_model для предсказания лучшего оператора
//         // ...
//     }
//     PlannerPolicy neural_policy = { neural_policy_choose };
//     planner_set_policy(&neural_policy);
// }

int main(void) {
    if (init_everything() != 0)
        return EXIT_FAILURE;

    if (wm_init(&global_wm,0,0) != 0)
        return EXIT_FAILURE;

    start_subconscious_daemon(&global_wm);

    while (g_running) {
        IPCPacket packet;

        IPCStatus st = ipc_receive(&packet);

        if (st == IPC_DISCONNECTED) {
            LOG_IPC("IPC disconnected.");
            break;
        }

        if (st != IPC_OK)
            continue;

        ipc_dispatch(&packet);
    }

    shutdown_everything();

    return EXIT_SUCCESS;
}
