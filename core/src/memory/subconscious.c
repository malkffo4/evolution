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

/*
 * Регистровый контракт InductiveExtractor <-> MetaCriticGraph:
 *   R12 = R_RULE_HEAD     — id первой инструкции сгенерированного правила
 *   R13 = R_PATTERN_COUNT — сила паттерна (сколько раз наблюдался)
 *   R14 = R_EVAL_STATUS   — VMStatus исполнения правила в песочнице
 * Эти три регистра — единственный публичный интерфейс между алгоритмами,
 * далее у каждого своё приватное окно.
 */
enum {
    R_RULE_HEAD     = 12,
    R_PATTERN_COUNT = 13,
    R_EVAL_STATUS   = 14,

    // --- приватное окно InductiveExtractor ---
    R_GOAL           = 44,
    R_GOAL_FOUND     = 45,
    R_MINCOUNT       = 46,
    R_PATTERN_PROC   = 47,
    R_PATTERN_SAMPLE = 48,
    R_PATTERN_FOUND  = 49,
    R_ZERO           = 50,
    R_SANDBOX_CTX    = 51,
    R_CAUSE          = 52,
    R_NEW_ATOM       = 53,
    R_MAXTICKS       = 58,
    R_META_ALGO      = 59,

    // --- регистры ВНУТРИ сгенерированного правила (используются только
    //     при его исполнении через OP_EVAL_GRAPH, не InductiveExtractor'ом) ---
    RULE_R_PROC    = 60,
    RULE_R_SUBJECT = 61,
    RULE_R_CTX     = 62,
    RULE_R_TMP     = 63,
};

static Pipeline* build_inductive_extractor_pipeline(node_id_t meta_critic_algo_id) {
    Pipeline *p = pipeline_create();
    if (!p) return NULL;

    p->constants.int_consts = malloc(4 * sizeof(int64_t));
    if (!p->constants.int_consts) { pipeline_free(p); return NULL; }
    p->constants.int_consts[0] = 0;                          // R_ZERO
    p->constants.int_consts[1] = 3;                           // R_MINCOUNT
    p->constants.int_consts[2] = 16;                          // R_MAXTICKS (лимит тиков песочницы)
    p->constants.int_consts[3] = (int64_t)meta_critic_algo_id;// R_META_ALGO
    p->constants.int_count = 4;

    union { float f; uint32_t u; } merge_thresh;
    merge_thresh.f = 0.60f;   // порог, начиная с которого MERGE_CTX закрепит правило

#define GEN_INSTR_FIELDS(sp_base, f0,f1,f2,f3,f4,f5) \
    { OP_WRITE_SP, .arg={ (sp_base)+0, (f0) } }, \
    { OP_WRITE_SP, .arg={ (sp_base)+1, (f1) } }, \
    { OP_WRITE_SP, .arg={ (sp_base)+2, (f2) } }, \
    { OP_WRITE_SP, .arg={ (sp_base)+3, (f3) } }, \
    { OP_WRITE_SP, .arg={ (sp_base)+4, (f4) } }, \
    { OP_WRITE_SP, .arg={ (sp_base)+5, (f5) } }

    Instruction code[] = {
        /*0*/  { OP_LOAD_CONST, .arg={R_ZERO, 0} },
        /*1*/  { OP_LOAD_CONST, .arg={R_MINCOUNT, 1} },
        /*2*/  { OP_LOAD_CONST, .arg={R_MAXTICKS, 2} },
        /*3*/  { OP_LOAD_CONST, .arg={R_META_ALGO, 3} },

        /*4*/  { OP_WM_TOP_GOAL, .arg={R_GOAL, R_GOAL_FOUND} },
        /*5*/  { OP_JGE, .arg={R_GOAL_FOUND, R_ZERO, 7} },
        /*6*/  { OP_HALT },

        /*7*/  { OP_MINE_CAUSAL_PATTERN, .arg={R_GOAL, R_MINCOUNT, R_PATTERN_PROC, R_PATTERN_SAMPLE, R_PATTERN_COUNT, R_PATTERN_FOUND} },
        /*8*/  { OP_JGE, .arg={R_PATTERN_FOUND, R_ZERO, 10} },
        /*9*/  { OP_HALT },

        /*10*/ { OP_SPAWN_CTX, .arg={R_SANDBOX_CTX} },
        /*11*/ { OP_MOVE, .arg={R_CAUSE, R_ZERO} },   // старт цепочки, cause=0

        // --- α: GLOAD_CONST dst=RULE_R_PROC, wide = R_PATTERN_PROC ---
        GEN_INSTR_FIELDS(0, RULE_R_PROC,0,0,0,0,0),
        /*18*/ { OP_ASSERT_INSTRUCTION, .arg={OP_GLOAD_CONST, 0, R_CAUSE, R_NEW_ATOM, R_PATTERN_PROC, 1} },
        /*19*/ { OP_MOVE, .arg={R_RULE_HEAD, R_NEW_ATOM} },   // запоминаем ГОЛОВУ правила
        /*20*/ { OP_MOVE, .arg={R_CAUSE, R_NEW_ATOM} },

        // --- β: GLOAD_CONST dst=RULE_R_SUBJECT, wide = R_GOAL ---
        GEN_INSTR_FIELDS(0, RULE_R_SUBJECT,0,0,0,0,0),
        /*27*/ { OP_ASSERT_INSTRUCTION, .arg={OP_GLOAD_CONST, 0, R_CAUSE, R_NEW_ATOM, R_GOAL, 1} },
        /*28*/ { OP_MOVE, .arg={R_CAUSE, R_NEW_ATOM} },

        // --- γ0: GLOAD_CONST dst=RULE_R_CTX, wide = 0 (базовый контекст) ---
        GEN_INSTR_FIELDS(0, RULE_R_CTX,0,0,0,0,0),
        /*35*/ { OP_ASSERT_INSTRUCTION, .arg={OP_GLOAD_CONST, 0, R_CAUSE, R_NEW_ATOM, R_ZERO, 0} },
        /*36*/ { OP_MOVE, .arg={R_CAUSE, R_NEW_ATOM} },

        // --- γ: QUERY(proc=RULE_R_PROC, participant=RULE_R_SUBJECT, ctx=RULE_R_CTX,
        //             sp_offset=0, count_dst=RULE_R_TMP, sti_off=0) ---
        GEN_INSTR_FIELDS(0, RULE_R_PROC, RULE_R_SUBJECT, RULE_R_CTX, 0, RULE_R_TMP, 0),
        /*43*/ { OP_ASSERT_INSTRUCTION, .arg={OP_QUERY, 0, R_CAUSE, R_NEW_ATOM, R_ZERO, 0} },
        /*44*/ { OP_MOVE, .arg={R_CAUSE, R_NEW_ATOM} },

        // --- δ: ASSERT(proc=RULE_R_PROC, arg0=RULE_R_SUBJECT, arg1=RULE_R_SUBJECT,
        //               dst=RULE_R_TMP) — сжатие N наблюдений в одно усиленное убеждение ---
        GEN_INSTR_FIELDS(0, RULE_R_PROC, RULE_R_SUBJECT, RULE_R_SUBJECT, RULE_R_TMP, 0, 0),
        /*51*/ { OP_ASSERT_INSTRUCTION, .arg={OP_ASSERT, 0, R_CAUSE, R_NEW_ATOM, R_ZERO, 0} },
        /*52*/ { OP_MOVE, .arg={R_CAUSE, R_NEW_ATOM} },

        // --- Исполнить сгенерированное правило В ПЕСОЧНИЦЕ (max_ticks=16) ---
        /*53*/ { OP_EVAL_GRAPH, .arg={R_RULE_HEAD, R_MAXTICKS, R_EVAL_STATUS} },

        // --- Вызвать Мета-Критика (тоже алгоритм из LMDB, читает R12/R13/R14) ---
        /*54*/ { OP_EXEC_ALGORITHM, .arg[0]=R_META_ALGO },

        // --- Критик уже скорректировал truth_confidence R_RULE_HEAD внутри
        //     песочницы. Схлопывание безусловно: порог отфильтрует сам. ---
        /*55*/ { OP_MERGE_CTX, .arg={merge_thresh.u} },
        /*56*/ { OP_HALT }
    };

#undef GEN_INSTR_FIELDS

    size_t num = sizeof(code) / sizeof(code[0]);
    p->code_len = (uint32_t)num;
    memcpy(p->code, code, sizeof(code));
    return p;
}

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

        static int induction_nudge_counter = 0;
        if (++induction_nudge_counter >= 6) {   // ~раз в минуту при tick=10с
            induction_nudge_counter = 0;
            wm_activate(&global_wm, djb2_hash("InductiveSynthesisGoal"), 0.8f, 0.7f);
            for (uint32_t i = 0; i < global_wm.count; i++)
                if (global_wm.nodes[i].node_id == djb2_hash("InductiveSynthesisGoal"))
                    global_wm.nodes[i].state.usefulness = 0.85f;
        }

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

enum { MC_R_ZERO = 16, MC_R_DELTA = 17 };

static Pipeline* build_meta_critic_pipeline(void) {
    Pipeline *p = pipeline_create();
    if (!p) return NULL;

    p->constants.int_consts = malloc(sizeof(int64_t));
    if (!p->constants.int_consts) { pipeline_free(p); return NULL; }
    p->constants.int_consts[0] = 0;
    p->constants.int_count = 1;

    p->constants.float_consts = malloc(2 * sizeof(double));
    if (!p->constants.float_consts) { pipeline_free(p); return NULL; }
    p->constants.float_consts[0] = 0.40;   // награда: правило исполнилось чисто
    p->constants.float_consts[1] = -0.30;  // штраф: сбой при исполнении в песочнице
    p->constants.float_count = 2;

    // Контракт: R12=R_RULE_HEAD, R14=R_EVAL_STATUS (заполнены InductiveExtractor'ом)
    Instruction code[] = {
        /*0*/ { OP_LOAD_CONST, .arg={MC_R_ZERO, 0} },
        /*1*/ { OP_JGE, .arg={14 /*R_EVAL_STATUS*/, MC_R_ZERO, 5} }, // status>0 (ошибка) -> штраф
        /*2*/ {  .arg={MC_R_DELTA, 0} },              // +0.40
        /*3*/ { OP_ATOM_REINFORCE, .arg={12 /*R_RULE_HEAD*/, MC_R_DELTA} },
        /*4*/ { OP_HALT },
        /*5*/ { OP_LOAD_FCONST, .arg={MC_R_DELTA, 1} },              // -0.30
        /*6*/ { OP_ATOM_REINFORCE, .arg={12 /*R_RULE_HEAD*/, MC_R_DELTA} },
        /*7*/ { OP_HALT }
    };

    p->code_len = sizeof(code) / sizeof(code[0]);
    memcpy(p->code, code, sizeof(code));
    return p;
}

static void ensure_meta_critic_exists(MDB_txn *txn) {
    node_id_t id = djb2_hash("MetaCriticGraph");
    Pipeline *existing = NULL;
    if (algorithm_load(txn, id, &existing) == 0 && existing) {
        pipeline_free(existing);
        return;   // уже загружен (в т.ч. возможно отредактирован вручную) — не трогаем
    }
    Pipeline *mc = build_meta_critic_pipeline();
    if (mc) {
        if (algorithm_save(txn, id, mc) != MDB_SUCCESS)
            LOG_ERROR("Failed to save MetaCriticGraph algorithm");
        pipeline_free(mc);
    }
}

static void ensure_inductive_extractor_exists(MDB_txn *txn) {
    node_id_t meta_id  = djb2_hash("MetaCriticGraph");
    node_id_t algo_id  = djb2_hash("InductiveExtractor");
    node_id_t goal_id  = djb2_hash("InductiveSynthesisGoal");

    Pipeline *existing = NULL;
    if (algorithm_load(txn, algo_id, &existing) == 0 && existing) {
        pipeline_free(existing);
        return;
    }

    Pipeline *ie = build_inductive_extractor_pipeline(meta_id);
    if (!ie) return;
    if (algorithm_save(txn, algo_id, ie) != MDB_SUCCESS)
        LOG_ERROR("Failed to save InductiveExtractor algorithm");
    pipeline_free(ie);

    // Самостоятельная привязка к Goal — без единого Python-вызова.
    if (global_hyper_mem) {
        hyper_memory_set_txn(global_hyper_mem, txn);

        NeuroAtom goal_type = {0};
        goal_type.id = hyper_memory_new_id(global_hyper_mem);
        goal_type.process_id = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
        goal_type.args[0].raw = HYPER_MAKE_REF(goal_id);
        goal_type.args[1].raw = HYPER_MAKE_REF(djb2_hash("Goal"));
        goal_type.truth_mean = 1.0f; goal_type.truth_confidence = 1.0f;
        hyper_assert_unique(global_hyper_mem, &goal_type);

        NeuroAtom link = {0};
        link.id = hyper_memory_new_id(global_hyper_mem);
        link.process_id = proc_make(djb2_hash("HAS_ALGORITHM"), PROC_KIND_RELATION);
        link.args[0].raw = HYPER_MAKE_REF(goal_id);
        link.args[1].raw = HYPER_MAKE_REF(algo_id);
        link.truth_mean = 1.0f; link.truth_confidence = 1.0f;
        hyper_assert_unique(global_hyper_mem, &link);
    }
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
    ensure_meta_critic_exists(txn);
    ensure_inductive_extractor_exists(txn);
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
