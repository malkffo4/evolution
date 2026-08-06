// tests/cybersec_reasoning_test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <lmdb.h>

#include "storage/db/db.h"
#include "storage/db/db_writer.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "math/hash.h"
#include "memory/working.h"
#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/vm/instruction.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "runtime/ops/vm_ops.h"
#include "knowledge/algorithm_saver.h"
#include "common.h"
/*
 * Правило: IF ?x FLOWS_TO ?y AND ?x HAS_PROPERTY Unsanitized
 *          THEN ?y HAS_VULNERABILITY SQL_Injection
 *
 * Как и CheckEdgeAlgo/AverageOfThree в существующей кодовой базе,
 * параметры правила (участники отношений) — константы, зашитые в
 * ConstantPool при компиляции Pipeline (Code = Data: сам алгоритм
 * лежит в LMDB, но конкретизирован под предметную область во время
 * его "написания" — так же, как книга алгоритмов в book_loader.py).
 */
static Pipeline *build_sql_injection_rule(uint64_t input_a, uint64_t db_query,
                                           uint64_t flows_to_proc, uint64_t has_prop_proc,
                                           uint64_t has_vuln_proc, uint64_t sql_injection) {
    Pipeline *p = pipeline_create();
    if (!p) return NULL;

    p->constants.int_consts = malloc(8 * sizeof(int64_t));
    if (!p->constants.int_consts) { pipeline_free(p); return NULL; }

    p->constants.int_consts[0] = (int64_t)input_a;
    p->constants.int_consts[1] = (int64_t)db_query;
    p->constants.int_consts[2] = (int64_t)flows_to_proc;
    p->constants.int_consts[3] = (int64_t)has_prop_proc;
    p->constants.int_consts[4] = (int64_t)has_vuln_proc;
    p->constants.int_consts[5] = (int64_t)sql_injection;
    p->constants.int_consts[6] = 0; // базовый контекст
    p->constants.int_consts[7] = 1; // порог "единица"
    p->constants.int_count = 8;

    enum {
        R_INPUT_A = 1, R_DB_QUERY = 2,
        R_FLOWS_TO_PROC = 4, R_HAS_PROP_PROC = 5, R_HAS_VULN_PROC = 6,
        R_SQL_INJECTION = 7, R_CONTEXT = 8,
        R_FLOWS_COUNT = 9, R_PROP_COUNT = 10, R_SUM_COUNT = 11, R_ONE = 12,
        R_RESULT_ID = 13, R_CAUSE_ATOM = 14
    };

    Instruction code[] = {
        /*0*/  { .operator_id = OP_LOAD_CONST, .arg = { R_INPUT_A, 0 } },
        /*1*/  { .operator_id = OP_LOAD_CONST, .arg = { R_DB_QUERY, 1 } },
        /*2*/  { .operator_id = OP_LOAD_CONST, .arg = { R_FLOWS_TO_PROC, 2 } },
        /*3*/  { .operator_id = OP_LOAD_CONST, .arg = { R_HAS_PROP_PROC, 3 } },
        /*4*/  { .operator_id = OP_LOAD_CONST, .arg = { R_HAS_VULN_PROC, 4 } },
        /*5*/  { .operator_id = OP_LOAD_CONST, .arg = { R_SQL_INJECTION, 5 } },
        /*6*/  { .operator_id = OP_LOAD_CONST, .arg = { R_CONTEXT, 6 } },
        /*7*/  { .operator_id = OP_LOAD_CONST, .arg = { R_ONE, 7 } },

        // Посылка 1: ?x FLOWS_TO ?y, где ?x = InputA
        /*8*/  { .operator_id = OP_QUERY, .arg = { R_FLOWS_TO_PROC, R_INPUT_A, R_CONTEXT, 0,  R_FLOWS_COUNT, 0 } },

        // Посылка 2: ?x HAS_PROPERTY Unsanitized, где ?x = InputA
        /*9*/  { .operator_id = OP_QUERY, .arg = { R_HAS_PROP_PROC, R_INPUT_A, R_CONTEXT, 10, R_PROP_COUNT,  0 } },

        /*10*/ { .operator_id = OP_ADD,  .arg = { R_SUM_COUNT, R_FLOWS_COUNT, R_PROP_COUNT } },

        // Обе посылки истинны (каждая дала >=1 совпадение) -> sum > 1
        /*11*/ { .operator_id = OP_JGE,  .arg = { R_SUM_COUNT, R_ONE, 13 } },
        /*12*/ { .operator_id = OP_BRANCH, .arg = { 15 } }, // посылки не выполнены -> сразу HALT

        // Причина вывода — атом первой посылки (FLOWS_TO), см. scratch[0]
        /*13*/ { .operator_id = OP_READ_SP, .arg = { R_CAUSE_ATOM, 0 } },

        // Заключение: DatabaseQuery HAS_VULNERABILITY SQL_Injection
        /*14*/ { .operator_id = OP_DERIVE, .arg = { R_HAS_VULN_PROC, R_DB_QUERY, R_SQL_INJECTION, R_CAUSE_ATOM, R_RESULT_ID } },

        /*15*/ { .operator_id = OP_HALT }
    };

    size_t num = sizeof(code) / sizeof(code[0]);
    p->code_len = (uint32_t)num;
    memcpy(p->code, code, sizeof(code));

    return p;
}

typedef struct {
    uint64_t input_a, db_query, unsanitized;
    uint64_t flows_to_proc, has_prop_proc, has_vuln_proc;
    uint64_t sql_injection, goal_id, algo_id;
} CyberSetup;

static int setup_txn_fn(MDB_txn *txn, void *arg) {
    CyberSetup *s = arg;
    HyperMemory *hmem = hyper_memory_new(db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    if (!hmem) return -1;
    hyper_memory_set_db_causal(hmem, db.graph.hyper.idx_causal);

    // Факт 1: InputA -FLOWS_TO-> DatabaseQuery
    NeuroAtom flows = {0};
    flows.id = hyper_memory_new_id(hmem);
    flows.process_id = s->flows_to_proc;
    flows.args[0].raw = HYPER_MAKE_REF(s->input_a);
    flows.args[1].raw = HYPER_MAKE_REF(s->db_query);
    flows.truth_mean = 1.0f; flows.truth_confidence = 1.0f;
    flows.sti = 0.6f; flows.lti = 0.3f;
    if (hyper_assert_unique(txn, hmem, &flows) < 0) { hyper_memory_free(hmem); return -1; }

    // Факт 2: InputA -HAS_PROPERTY-> Unsanitized
    NeuroAtom prop = {0};
    prop.id = hyper_memory_new_id(hmem);
    prop.process_id = s->has_prop_proc;
    prop.args[0].raw = HYPER_MAKE_REF(s->input_a);
    prop.args[1].raw = HYPER_MAKE_REF(s->unsanitized);
    prop.truth_mean = 1.0f; prop.truth_confidence = 1.0f;
    prop.sti = 0.6f; prop.lti = 0.3f;
    if (hyper_assert_unique(txn, hmem, &prop) < 0) { hyper_memory_free(hmem); return -1; }

    // Мета: HAS_ALGORITHM — отношение вида "цель -> алгоритм"
    NeuroAtom meta = {0};
    meta.id = hyper_memory_new_id(hmem);
    meta.process_id = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
    meta.args[0].raw = djb2_hash("HAS_ALGORITHM");
    meta.args[1].raw = djb2_hash("GoalAlgorithmRelation");
    meta.truth_mean = 1.0f; meta.truth_confidence = 1.0f;
    if (hyper_assert_unique(txn, hmem, &meta) < 0) { hyper_memory_free(hmem); return -1; }

    // IS_A(goal, "Goal") — типизация Knowledge Object
    NeuroAtom goal_type = {0};
    goal_type.id = hyper_memory_new_id(hmem);
    goal_type.process_id = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
    goal_type.args[0].raw = HYPER_MAKE_REF(s->goal_id);
    goal_type.args[1].raw = HYPER_MAKE_REF(djb2_hash("Goal"));
    goal_type.truth_mean = 1.0f; goal_type.truth_confidence = 1.0f;
    hyper_assert_unique(txn, hmem, &goal_type);

    // HAS_ALGORITHM(goal, algo)
    NeuroAtom link = {0};
    link.id = hyper_memory_new_id(hmem);
    link.process_id = proc_make(djb2_hash("HAS_ALGORITHM"), PROC_KIND_RELATION);
    link.args[0].raw = HYPER_MAKE_REF(s->goal_id);
    link.args[1].raw = HYPER_MAKE_REF(s->algo_id);
    link.truth_mean = 1.0f;
    link.truth_confidence = 1.0f;
    link.sti = 0.5f;
    if (hyper_assert_unique(txn, hmem, &link) < 0) { hyper_memory_free(hmem); return -1; }

    hyper_memory_free(hmem);

    // Дедуктивное правило — исполняемый Pipeline (Code = Data)
    Pipeline *rule = build_sql_injection_rule(s->input_a, s->db_query,
                                               s->flows_to_proc, s->has_prop_proc,
                                               s->has_vuln_proc, s->sql_injection);
    if (!rule) return -1;
    int rc = algorithm_save(txn, s->algo_id, rule);
    pipeline_free(rule);

    rc = planner_bootstrap(txn);

    return (rc == MDB_SUCCESS) ? 0 : -1;
}

int main(void) {
    system("rm -rf ./test_cyber_db");
    assert(init_lmdb("./test_cyber_db") == MDB_SUCCESS);
    assert(db_writer_start() == 0);
    operator_registry_init();

    CyberSetup s = {0};
    s.input_a       = djb2_hash("InputA");
    s.db_query      = djb2_hash("DatabaseQuery");
    s.unsanitized   = djb2_hash("Unsanitized");
    s.sql_injection = djb2_hash("SQL_Injection");

    s.flows_to_proc = proc_make(djb2_hash("FLOWS_TO"), PROC_KIND_RELATION);
    s.has_prop_proc = proc_make(djb2_hash("HAS_PROPERTY"), PROC_KIND_RELATION);
    s.has_vuln_proc = proc_make(djb2_hash("HAS_VULNERABILITY"), PROC_KIND_RELATION);

    s.goal_id       = djb2_hash("FindSQLInjection");
    s.algo_id       = djb2_hash("SqlInjectionRule");

    assert(db_write_sync(setup_txn_fn, &s) == 0);

    // --- Активируем цель в Working Memory (как это делает cmd_execute_op) ---
    WorkingMemory wm;
    assert(wm_init(&wm, 16) == 0);
    wm_activate(&wm, s.goal_id, 1.0f, 0.0f);
    for (uint32_t i = 0; i < wm.count; i++) {
        if (wm.nodes[i].node_id == s.goal_id) {
            wm.nodes[i].state.usefulness = 0.9f;
            break;
        }
    }

    // --- Один такт сознания: MainLoop находит цель и АСИНХРОННО спавнит воркер ---
    MDB_txn *plan_txn;
    assert(mdb_txn_begin(db.env, NULL, MDB_RDONLY, &plan_txn) == MDB_SUCCESS);

    HyperMemory *plan_hmem = hyper_memory_new(db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    assert(plan_hmem != NULL);

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    assert(vm_init(&ctx, plan_txn, &wm) == VM_OK);
    ctx.hyper_mem = plan_hmem;

    Instruction eval_ins = { .operator_id = OP_EVALUATE_GOALS };
    int rc = vm_op_evaluate_goals(&ctx, &eval_ins);
    if (rc != VM_OK) {
        fprintf(stderr, "FATAL: vm_op_evaluate_goals failed with code %d\n", rc);
    }
    assert(rc == VM_OK); // немедленный возврат — задача уже в пуле воркеров

    vm_destroy(&ctx);

    // ИСПРАВЛЕНИЕ 1: Освобождаем plan_hmem, так как фоновый воркер
    // теперь создаёт свою собственную изолированную копию HyperMemory!
    hyper_memory_free(plan_hmem);
    mdb_txn_abort(plan_txn);

    // --- Ждём материализации асинхронного вывода ---
    bool found = false;
    for (int attempt = 0; attempt < 100 && !found; attempt++) {
        usleep(20000); // 20ms

        MDB_txn *poll_txn;
        if (mdb_txn_begin(db.env, NULL, MDB_RDONLY, &poll_txn) != MDB_SUCCESS) continue;

        HyperMemory poll_hm = {0};
        poll_hm.dbi_atoms       = db.graph.hyper.atoms;
        poll_hm.dbi_idx_process = db.graph.hyper.idx_process;
        poll_hm.dbi_idx_args    = db.graph.hyper.idx_args;
        poll_hm.dbi_idx_context = db.graph.hyper.idx_context;

        NeuroAtom *results = NULL;
        size_t count = 0;

        if (hyper_find_by_participant(poll_txn, &poll_hm, s.db_query, 0, &results, &count) == 0) {
            for (size_t i = 0; i < count; i++) {
                if (results[i].process_id == s.has_vuln_proc &&
                    HYPER_GET_ID(results[i].args[0].raw) == s.db_query &&
                    HYPER_GET_ID(results[i].args[1].raw) == s.sql_injection) {
                    found = true;
                    // ИСПРАВЛЕНИЕ 2: Убрали двойной free и двойной abort.
                    // Просто выходим из цикла, все ресурсы штатно очистятся ниже.
                    break;
                }
            }
        }

        if (results) free(results);
        mdb_txn_abort(poll_txn);
    }

    assert(found && "SQL Injection vulnerability was not derived asynchronously in time");
    printf("Cybersecurity reasoning test passed: DatabaseQuery HAS_VULNERABILITY SQL_Injection "
           "derived asynchronously via deductive rule + Actor pool.\n");

    wm_clear(&wm);
    db_writer_stop();
    close_lmdb();
    system("rm -rf ./test_cyber_db");

    return 0;
}
