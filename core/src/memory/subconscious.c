// memory/subconscious.c
#include <stddef.h>
#include <stdint.h>
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "core/globals.h"
#include "subconscious.h"
#include "critic_state.h"
#include "storage/db/db.h"
#include "storage/string_pool/string_pool.h"
#include "storage/db/db_writer.h"
#include "memory/working.h"
#include "memory/decay.h"
#include "reasoning/planner.h"
#include "knowledge/algorithm_loader.h"
#include "knowledge/algorithm_saver.h"
#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/ops/opcode.h"
#include "runtime/logging/logging.h"
#include "math/hash.h"

static ResearchTask task_queue[MAX_PENDING_TASKS];
static int task_count = 0;
static pthread_mutex_t task_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile int g_think_trigger = 0;

static int dmn_running = 0;
static pthread_t dmn_thread;
static uint64_t main_loop_algo_id = 0;

static pthread_t decay_timer_thread;
static volatile int decay_timer_running = 0;

static int decay_txn_fn(MDB_txn *txn, void *arg) {
    (void)arg;
    if (!global_hyper_mem) return -1;
    hyper_memory_set_txn(global_hyper_mem, txn);

    MDB_stat stat;
    uint64_t total_atoms = 0;
    if (mdb_stat(txn, db.graph.hyper.atoms, &stat) == MDB_SUCCESS)
        total_atoms = (uint64_t)stat.ms_entries;

    homeostasis_step(&g_homeostasis, &HOMEOSTASIS_DEFAULT, &global_wm, total_atoms);

    DecayStats stats;
    return subconscious_decay_cycle(global_hyper_mem, &g_homeostasis.policy, &stats);
}

static void *decay_timer_loop(void *arg) {
    (void)arg;
    while (decay_timer_running && g_running) {
        struct timespec ts = {10, 0}; // раз в 10 секунд, независимо от MainLoop
        nanosleep(&ts, NULL);
        if (!decay_timer_running || !g_running) break;

        int rc = db_write_sync(decay_txn_fn, NULL);
        if (rc != 0) LOG_WARN("[SUBCONSCIOUS] Timed decay cycle failed rc=%d", rc);
    }
    return NULL;
}

void start_decay_timer(void) {
    decay_timer_running = 1;
    pthread_create(&decay_timer_thread, NULL, decay_timer_loop, NULL);
}

void stop_decay_timer(void) {
    decay_timer_running = 0;
    pthread_join(decay_timer_thread, NULL);
}


void enqueue_research_task(uint64_t node_id, const char *query) {
    pthread_mutex_lock(&task_mutex);
    if (task_count < MAX_PENDING_TASKS) {
        task_queue[task_count].node_id = node_id;
        strncpy(task_queue[task_count].query, query, sizeof(task_queue[task_count].query) - 1);
        task_count++;
    }
    pthread_mutex_unlock(&task_mutex);
}

int get_pending_tasks(ResearchTask *buffer, int max_count) {
    int cnt = 0;
    pthread_mutex_lock(&task_mutex);
    cnt = (task_count < max_count) ? task_count : max_count;
    memcpy(buffer, task_queue, (size_t)cnt * sizeof(ResearchTask));
    if (cnt > 0) {
        memmove(task_queue, task_queue + cnt, (size_t)(task_count - cnt) * sizeof(ResearchTask));
        task_count -= cnt;
    }
    pthread_mutex_unlock(&task_mutex);
    return cnt;
}

static Pipeline* build_critic_main_pipeline(void) {
    Pipeline *p = pipeline_create();
    if (!p) return NULL;

    Instruction code[] = {
        { .operator_id = OP_CRITIC_APPLY },
        { .operator_id = OP_HALT }
    };

    size_t num = sizeof(code)/sizeof(code[0]);
    memcpy(p->code, code, sizeof(code));
    p->code_len = (uint32_t)num;

    return p;
}

static uint64_t g_critic_main_algo_id = 0;

static void ensure_critic_main_exists(MDB_txn *txn) {
    g_critic_main_algo_id = djb2_hash("CriticMain");

    Pipeline *existing = NULL;
    if (algorithm_load(txn, g_critic_main_algo_id, &existing) == 0) {
        if (existing) pipeline_free(existing);
        return;
    }
    Pipeline *cm = build_critic_main_pipeline();
    if (cm) {
        if (algorithm_save(txn, g_critic_main_algo_id, cm) != MDB_SUCCESS)
            LOG_ERROR("Failed to save CriticMain algorithm");
        pipeline_free(cm);
    }
}

static Pipeline* build_main_loop_pipeline(void) {
    Pipeline *p = pipeline_create();
    if (!p) return NULL;

    // Ограниченный цикл сознания: за один вызов vm_execute() MainLoop
    // многократно пересканирует Working Memory и асинхронно диспетчеризует
    // найденные цели, затем один раз вызывает CriticMain и завершается (OP_HALT).
    #define MAIN_LOOP_TICKS_PER_INVOCATION 16

    // g_critic_main_algo_id — 64-битный djb2-хэш (до 62 значащих бит согласно
    // HYPER_VALUE_MASK). Передаём его через ConstantPool (int64_t, полный
    // диапазон), а не напрямую как .arg[0] (uint32_t) — иначе старшие биты
    // молча обрезаются и OP_CALL получает несуществующий pipeline ID.
    p->constants.int_consts = malloc(4 * sizeof(int64_t));
    if (!p->constants.int_consts) {
        pipeline_free(p);
        return NULL;
    }
    p->constants.int_consts[0] = MAIN_LOOP_TICKS_PER_INVOCATION; // счётчик
    p->constants.int_consts[1] = 0;                              // ноль
    p->constants.int_consts[2] = 1;                               // единица
    p->constants.int_consts[3] = (int64_t)g_critic_main_algo_id;  // ID CriticMain, БЕЗ усечения
    p->constants.int_count = 4;
    p->constants.float_consts = NULL;
    p->constants.float_count = 0;
    p->constants.str_consts = NULL;
    p->constants.str_count = 0;

    // Регистры зарезервированы за MainLoop и не пересекаются с регистрами
    // диспетчеризуемых алгоритмов (каждый исполняется в отдельном
    // изолированном VMContext воркера, см. vm_pool.c).
    enum { R_COUNTER = 10, R_ZERO = 11, R_ONE = 12, R_CRITIC_ALGO = 13 };

    Instruction code[] = {
        /*0*/  { .operator_id = OP_LOAD_CONST, .arg = { R_COUNTER,     0 } },
        /*1*/  { .operator_id = OP_LOAD_CONST, .arg = { R_ZERO,        1 } },
        /*2*/  { .operator_id = OP_LOAD_CONST, .arg = { R_ONE,         2 } },
        /*3*/  { .operator_id = OP_LOAD_CONST, .arg = { R_CRITIC_ALGO, 3 } },
        /*4*/  { .operator_id = OP_LOAD_CONTEXT },                          // loop_start
        /*5*/  { .operator_id = OP_EVALUATE_GOALS, .flags = INS_FLAG_SOFT_FAIL },
        /*6*/  { .operator_id = OP_SPREAD_ACTIVATION },
        /*7*/  { .operator_id = OP_SUB, .arg = { R_COUNTER, R_COUNTER, R_ONE } },
        /*8*/  { .operator_id = OP_JGE, .arg = { R_COUNTER, R_ZERO, 4 } },
        /*9*/  { .operator_id = OP_EXEC_ALGORITHM, .arg[0] = R_CRITIC_ALGO },
        /*10*/ { .operator_id = OP_HALT }
    };

    size_t num = sizeof(code) / sizeof(code[0]);
    p->code_len = (uint32_t)num;
    memcpy(p->code, code, sizeof(code));

    #undef MAIN_LOOP_TICKS_PER_INVOCATION
    return p;
}

static void ensure_main_loop_exists(MDB_txn *txn) {
    main_loop_algo_id = djb2_hash("MainLoop");

    Pipeline *existing = NULL;
    if (algorithm_load(txn, main_loop_algo_id, &existing) == 0) {
        if (existing) {
            pipeline_free(existing);
        }
        return;
    }

    Pipeline *ml = build_main_loop_pipeline();
    if (ml) {
        int rc = algorithm_save(txn, main_loop_algo_id, ml);
        if (rc != MDB_SUCCESS)
            LOG_ERROR("main_loop_algo_id not saved.");
        pipeline_free(ml);
    }
}

static Pipeline* build_core_planner_pipeline(void) {
    Pipeline *p = pipeline_create();
    if (!p) return NULL;

    /*
     * CorePlanner (Cognitive Cycle, шаг Plan) — 9 инструкций вместо
     * захардкоженного C-фолбэка в vm_op_evaluate_goals():
     *
     *   R_GOAL, R_FOUND      = OP_WM_TOP_GOAL()
     *   нет цели             -> HALT
     *   R_ALGO, R_ALGO_FOUND = OP_SELECT_ALGORITHM(R_GOAL)
     *   нет алгоритма        -> HALT (OP_SELECT_ALGORITHM уже поставил
     *                                  research-задачу и cooldown)
     *   OP_DISPATCH_ASYNC(R_GOAL, R_ALGO)  -> vm_pool, новый поток
     *   HALT
     */
    enum { R_GOAL = 20, R_FOUND = 21, R_ALGO = 22, R_ALGO_FOUND = 23, R_ZERO = 24 };

    p->constants.int_consts = malloc(sizeof(int64_t));
    if (!p->constants.int_consts) { pipeline_free(p); return NULL; }
    p->constants.int_consts[0] = 0;
    p->constants.int_count = 1;

    Instruction code[] = {
        /*0*/ { .operator_id = OP_LOAD_CONST,      .arg = { R_ZERO, 0 } },
        /*1*/ { .operator_id = OP_WM_TOP_GOAL,      .arg = { R_GOAL, R_FOUND } },
        /*2*/ { .operator_id = OP_JGE,              .arg = { R_FOUND, R_ZERO, 4 } },
        /*3*/ { .operator_id = OP_HALT },
        /*4*/ { .operator_id = OP_SELECT_ALGORITHM, .arg = { R_GOAL, R_ALGO, R_ALGO_FOUND } },
        /*5*/ { .operator_id = OP_JGE,              .arg = { R_ALGO_FOUND, R_ZERO, 7 } },
        /*6*/ { .operator_id = OP_HALT },
        /*7*/ { .operator_id = OP_DISPATCH_ASYNC,   .arg = { R_GOAL, R_ALGO } },
        /*8*/ { .operator_id = OP_HALT }
    };

    size_t num = sizeof(code) / sizeof(code[0]);
    p->code_len = (uint32_t)num;
    memcpy(p->code, code, sizeof(code));

    return p;
}

static void ensure_core_planner_exists(MDB_txn *txn) {
    uint64_t core_planner_id = djb2_hash("CorePlanner");

    Pipeline *existing = NULL;
    if (algorithm_load(txn, core_planner_id, &existing) == 0 && existing) {
        bool is_stub = true;
        for (uint32_t i = 0; i < existing->code_len; i++) {
            if (existing->code[i].operator_id != OP_HALT) { is_stub = false; break; }
        }
        pipeline_free(existing);
        if (!is_stub) return;   // уже настоящий планировщик — не трогаем
    }

    Pipeline *cp = build_core_planner_pipeline();
    if (cp) {
        if (algorithm_save(txn, core_planner_id, cp) != MDB_SUCCESS)
            LOG_ERROR("Failed to save CorePlanner algorithm");
        pipeline_free(cp);
    }
}

static Pipeline* build_analogy_planner_pipeline(void) {
    Pipeline *p = pipeline_create();
    if (!p) return NULL;

    /*
     * AnalogyPlanner — структурная аналогия как обычный Pipeline, не C
     * (docs/06_Reasoning.md: "Аналогия никогда не создаёт Fact напрямую.
     * Она создаёт только Hypothesis"). Та же связка операторов, что
     * доказана в core/tests/hypothesis_analogy_test.c:
     *   OP_GET_NEIGHBORS -> OP_READ_SP -> OP_FIND_SIMILAR ->
     *   OP_GET_NEIGHBORS -> OP_READ_SP -> OP_CONCAT_PATHS
     * плюс OP_WM_TOP_GOAL как источник стартового узла (вместо ручной
     * подстановки A/D в тесте) и OP_DERIVE, материализующий вывод как
     * настоящий Hypothesis NeuroAtom, а не только отладочную строку.
     *
     * Схема:
     *   R_GOAL --R_REL--> R_NEIGHBOR             (факт из графа, кэш edges)
     *   R_NEIGHBOR ~ R_ANALOG                     (косинусное сходство embedding)
     *   R_ANALOG --R_REL--> R_ANALOG_NB           (факт из графа, кэш edges)
     *   ───────────────────────────────────────────────────────────
     *   HYPOTHESIS: R_GOAL --R_REL--> R_ANALOG_NB (перенос по аналогии)
     *
     * R_REL — обычная константа ConstantPool[0], НЕ C-логика: чтобы
     * применить аналогию к другому типу отношений, достаточно пересохранить
     * этот Pipeline с другим int_consts[0], без единой строчки C.
     *
     * R_THRESHOLD сознательно НЕ грузится через OP_LOAD_CONST: этот
     * оператор умеет производить только REG_INT (см. vm_op_load_const,
     * runtime/ops/memory_ops.c — он никогда не читает constants.float_consts),
     * а vm_op_find_similar смотрит на REG_FLOAT в arg[1] и иначе просто
     * использует собственный дефолт 0.7f. Регистр остаётся REG_EMPTY после
     * memset() в vm_init() — это явное "порог не задан", а не скрытый баг.
     *
     * OP_FIND_SIMILAR НЕ помечен INS_FLAG_SOFT_FAIL: если аналогия не
     * находится (нет эмбеддинга / ничего не проходит порог), VM_NOT_FOUND
     * честно обрывает Pipeline. Это обычный неудачный когнитивный опыт —
     * vm_pool.c::vm_worker_txn_fn() преобразует его в outcome=0.0 для
     * score_update(), и неудачные попытки аналогии учат систему так же,
     * как удачные.
     *
     * REF-теги: atom.args[N].raw в OP_DERIVE берётся из регистра "как есть"
     * (vm_op_derive НЕ вызывает HYPER_MAKE_REF сам). Здесь это безопасно без
     * явного оборачивания: R_GOAL/R_ANALOG_NB всегда происходят от
     * djb2_hash()/HYPER_GET_ID(), которые уже маскируют значение до
     * HYPER_VALUE_MASK (62 бита, старшие 2 бита нулевые) — ровно тот же
     * битовый паттерн, что и HYPER_TYPE_REF (0b00). Это инвариант
     * hash.c/hyper_atom.h, а не совпадение.
     *
     * ВАЖНО (эксплуатационное ограничение): OP_GET_NEIGHBORS читает
     * ctx->preloaded_edges, заполняемый OP_LOAD_CONTEXT из ctx->memory.wm.
     * Запущенный через OP_DISPATCH_ASYNC (vm_pool_submit), воркер получает
     * СВЕЖИЙ ПУСТОЙ local_wm (vm_pool.c::vm_worker_txn_fn) — ни одного
     * активного узла, поэтому OP_WM_TOP_GOAL здесь найдёт R_GOAL_FOUND=0 и
     * Pipeline безобидно завершится по HALT на IP 4 с VM_OK (что
     * vm_worker_txn_fn запишет как outcome=1.0 — "успех", хотя реально
     * ничего не произошло). Чтобы AnalogyPlanner реально что-то находил,
     * его нужно вызывать СИНХРОННО через OP_EXEC_ALGORITHM из кадра,
     * который уже разделяет заполненный ctx->memory.wm — ровно так, как
     * MainLoop сам вызывает CriticMain (IP 9 в build_main_loop_pipeline()),
     * наследуя preloaded_edges от собственного OP_LOAD_CONTEXT (IP 4).
     * Эта функция только регистрирует Pipeline в LMDB, как и требовалось;
     * подключение его в исполнение MainLoop — отдельное решение, сознательно
     * не делается автоматически в этом патче.
     */
    p->constants.int_consts = malloc(2 * sizeof(int64_t));
    if (!p->constants.int_consts) { pipeline_free(p); return NULL; }
    p->constants.int_consts[0] = (int64_t)djb2_hash("CAUSES"); // R_REL: тип связи для обхода
    p->constants.int_consts[1] = 0;                             // R_ZERO
    p->constants.int_count = 2;

    enum {
        R_REL = 30, R_ZERO = 31,
        R_GOAL = 32, R_GOAL_FOUND = 33,
        R_NB_COUNT = 34, R_NEIGHBOR = 35,
        R_THRESHOLD = 36, R_ANALOG = 37,
        R_ANALOG_NB = 38, R_CAUSE = 39, R_HYP_ID = 40
    };
    enum { SP_NEIGHBORS = 0, SP_ANALOG_NEIGHBORS = 30, SP_PATH_STR = 50 };

    Instruction code[] = {
        /*0*/  { .operator_id = OP_LOAD_CONST,   .arg = { R_REL, 0 } },
        /*1*/  { .operator_id = OP_LOAD_CONST,   .arg = { R_ZERO, 1 } },
        /*2*/  { .operator_id = OP_WM_TOP_GOAL,   .arg = { R_GOAL, R_GOAL_FOUND } },
        /*3*/  { .operator_id = OP_JGE,           .arg = { R_GOAL_FOUND, R_ZERO, 5 } },
        /*4*/  { .operator_id = OP_HALT },   // WM пуста — нечего обобщать в этом тике
        /*5*/  { .operator_id = OP_GET_NEIGHBORS, .arg = { R_GOAL, R_REL, SP_NEIGHBORS, R_NB_COUNT } },
        /*6*/  { .operator_id = OP_READ_SP,       .arg = { R_NEIGHBOR, SP_NEIGHBORS } },
        /*7*/  { .operator_id = OP_FIND_SIMILAR,  .arg = { R_NEIGHBOR, R_THRESHOLD, R_ANALOG } },
        /*8*/  { .operator_id = OP_GET_NEIGHBORS, .arg = { R_ANALOG, R_REL, SP_ANALOG_NEIGHBORS, R_NB_COUNT } },
        /*9*/  { .operator_id = OP_READ_SP,       .arg = { R_ANALOG_NB, SP_ANALOG_NEIGHBORS } },
        /*10*/ { .operator_id = OP_CONCAT_PATHS,  .arg = { SP_PATH_STR, R_GOAL, R_NEIGHBOR, R_ANALOG, R_ANALOG_NB } },
        /*11*/ { .operator_id = OP_DERIVE,        .arg = { R_REL, R_GOAL, R_ANALOG_NB, R_CAUSE, R_HYP_ID } },
        /*12*/ { .operator_id = OP_HALT }
    };

    size_t num = sizeof(code) / sizeof(code[0]);
    p->code_len = (uint32_t)num;
    memcpy(p->code, code, sizeof(code));

    return p;
}

static void ensure_analogy_planner_exists(MDB_txn *txn) {
    uint64_t analogy_planner_id = djb2_hash("AnalogyPlanner");

    Pipeline *existing = NULL;
    if (algorithm_load(txn, analogy_planner_id, &existing) == 0) {
        if (existing) pipeline_free(existing);
        return; // уже загружен — не перезаписываем (если кто-то отредактировал
                // его через тулинг, повторная загрузка стёрла бы правки)
    }

    Pipeline *ap = build_analogy_planner_pipeline();
    if (ap) {
        if (algorithm_save(txn, analogy_planner_id, ap) != MDB_SUCCESS)
            LOG_ERROR("Failed to save AnalogyPlanner algorithm");
        pipeline_free(ap);
    }
}

/*
 * ---------------------------------------------------------------------
 * ПРОБЛЕМА 2: единый write-тик MainLoop, строго через db_writer.
 * ---------------------------------------------------------------------
 * Раньше dmn_loop() открывал write-транзакцию напрямую (mdb_txn_begin),
 * нарушая инвариант "любая write-транзакция LMDB — только внутри потока
 * db_writer" (docs/ARCHITECTURE.md). LMDB не даёт двум писателям работать
 * параллельно (env-lock их бы всё равно сериализовал), так что данные не
 * бились, но это был второй, необъявленный путь записи в обход единственной
 * легальной очереди.
 *
 * Вся логика одного тика теперь — DbWriteFn, вызываемый через
 * db_write_sync(). Критическая тонкость: сон при карантине (1 секунда) НЕ
 * живёт внутри DbWriteFn — иначе он держал бы поток db_writer (единственный
 * писатель на весь процесс) целую секунду, блокируя ВСЕ остальные очереди на
 * запись (IPC "learn", эпизоды vm_pool-воркеров). main_loop_tick_txn_fn()
 * только сообщает через out-параметр факт карантина; сам sleep выполняется
 * в dmn_loop() на его собственном потоке, уже после того как db_write_sync()
 * вернул управление и писатель освобождён для других задач.
 */
typedef struct {
    bool executed_ok;   // MainLoop реально отработал и вернул VM_OK в этом тике?
    bool quarantined;   // MainLoop был в карантине — не исполнялся вовсе?
} MainLoopTickResult;

static int main_loop_tick_txn_fn(MDB_txn *txn, void *arg) {
    MainLoopTickResult *result = arg;
    result->executed_ok = false;
    result->quarantined = false;

    if (global_hyper_mem)
        hyper_memory_set_txn(global_hyper_mem, txn);

    ensure_critic_main_exists(txn);
    ensure_core_planner_exists(txn);
    ensure_analogy_planner_exists(txn);
    ensure_main_loop_exists(txn);

    // Карантин проверяется ПОСЛЕ ensure_*_exists, но теперь при срабатывании
    // транзакция всё равно коммитится (return 0), а не абортится — свежие
    // Pipeline'ы (актуально на самом первом тике процесса) не выбрасываются.
    if (is_quarantined(main_loop_algo_id)) {
        result->quarantined = true;
        return 0;
    }

    Pipeline *main_loop = NULL;
    if (algorithm_load(txn, main_loop_algo_id, &main_loop) == 0 && main_loop) {
        VMContext ctx;
        memset(&ctx, 0, sizeof(ctx));

        int rc = vm_init(&ctx, txn, &global_wm);
        if (rc == VM_OK) {
            ctx.hyper_mem = global_hyper_mem;
            ctx.current_context = 0;
            ctx.current_episode_id = 0;

            rc = vm_execute(&ctx, main_loop);

            // Делегируем анализ успеха/провала Критику
            record_execution_result(main_loop_algo_id, rc);

            if (rc == VM_OK) {
                result->executed_ok = true;
            } else if (rc == VM_NOT_FOUND) {
                // нет целей – цикл был холостым, это не ошибка
            } else {
                LOG_DEBUG("MainLoop execution halted with status %d", rc);
            }
            vm_destroy(&ctx);
        } else {
            LOG_ERROR("Error vm_init()");
        }
        pipeline_free(main_loop);
    }

    // Всегда коммитим: и удачный, и холостой, и проваленный тик — валидный
    // персистентный опыт (тот же принцип, что в vm_pool.c::vm_worker_txn_fn).
    return 0;
}

/* -----------------------------------------------
 * Основной цикл демона
 * ----------------------------------------------- */
void* dmn_loop(void* arg) {
    (void)arg;
    int idle_cycles = 0;

    while (dmn_running && g_running) {
        // Уступаем процессор, если нет явного триггера
        if (!g_think_trigger) {
            int delay_ms = 100 * (1 << (idle_cycles > 5 ? 5 : idle_cycles));
            struct timespec ts = {delay_ms / 1000, (delay_ms % 1000) * 1000000};
            nanosleep(&ts, NULL);
        } else {
            g_think_trigger = 0;
        }

        if (!dmn_running || !g_running) break;

        MainLoopTickResult tick_result = {0};
        int rc = db_write_sync(main_loop_tick_txn_fn, &tick_result);

        if (rc != 0) {
            // Инфраструктурная ошибка db_writer (остановлен/очередь полна/
            // commit не удался) — не паника, штатное while-условие само
            // корректно завершит цикл при shutdown на следующей итерации.
            LOG_WARN("[SUBCONSCIOUS] MainLoop tick: db_write_sync failed rc=%d", rc);
            continue;
        }

        if (tick_result.quarantined) {
            // Сон держит ИМЕННО поток dmn_loop — писатель уже свободен,
            // транзакция закоммичена и завершена внутри db_write_sync()
            // до возврата сюда.
            struct timespec ts = {1, 0};
            nanosleep(&ts, NULL);
            idle_cycles++;
            continue;
        }

        if (tick_result.executed_ok)
            idle_cycles = 0;
        else
            idle_cycles++;
    }

    LOG_MEMORY("Subconscious daemon stopped.");
    return NULL;
}

void start_subconscious_daemon() {
    if (dmn_running) return;
    dmn_running = 1;
    init_quarantine();
    pthread_create(&dmn_thread, NULL, dmn_loop, NULL);
}

void stop_subconscious_daemon(void) {
    if (!dmn_running) return;
    LOG_MEMORY("Stopping subconscious daemon...");
    dmn_running = 0;
    pthread_join(dmn_thread, NULL);
    LOG_MEMORY("Subconscious daemon stopped.");
}