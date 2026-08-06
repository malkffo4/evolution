// core/tests/olympiad_composition_test.c
// "Олимпиадный" тест композиции алгоритмов: один VM-алгоритм
// (OuterCompose) ДВАЖДЫ вызывает другой (InnerDerive) через
// OP_EXEC_ALGORITHM, протягивая причинную связь первого вызова во
// второй. Это доказывает:
//   1) алгоритмы реально компонуются (subroutine-вызов через Native
//      Dispatch, а не хардкод на C);
//   2) общий регистровый файл позволяет составным алгоритмам передавать
//      состояние (id атома от предыдущего DERIVE) через границу вызова;
//   3) объяснимость (OP_TRACE) и credit assignment
//      (score_propagate_credit) охватывают ВЕСЬ многошаговый,
//      многоалгоритменный причинный путь: FIRE -> SMOKE -> ALARM.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <lmdb.h>

#include "runtime/vm/vm.h"
#include "runtime/vm/vm_context.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"
#include "runtime/ops/opcode.h"
#include "runtime/ops/vm_ops.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "knowledge/algorithm_saver.h"
#include "knowledge/evaluation.h"
#include "math/hash.h"

int main(void) {
    system("rm -rf ./test_olympiad_db");
    assert(init_lmdb("./test_olympiad_db") == MDB_SUCCESS);
    operator_registry_init();

    MDB_txn *txn;
    assert(mdb_txn_begin(db.env, NULL, 0, &txn) == 0);

    HyperMemory *hmem = hyper_memory_new(db.graph.hyper.atoms, db.graph.hyper.idx_process,
        db.graph.hyper.idx_args, db.graph.hyper.idx_context);
    assert(hmem != NULL);
    hyper_memory_set_db_causal(hmem, db.graph.hyper.idx_causal);

    node_id_t FIRE   = djb2_hash("OlympiadFIRE");
    node_id_t SMOKE  = djb2_hash("OlympiadSMOKE");
    node_id_t ALARM  = djb2_hash("OlympiadALARM");
    node_id_t CAUSES = djb2_hash("OlympiadCAUSES");
    node_id_t inner_algo_id = djb2_hash("InnerDerive_Olympiad");

    /* --- Алгоритм-подпрограмма: DERIVE(r1,r2,r3,cause=r4) -> id в r6 --- */
    Instruction inner_code[] = {
        { .operator_id = OP_DERIVE, .arg = {1, 2, 3, 4, 6} },
        { .operator_id = OP_HALT }
    };
    Pipeline inner_pl = { .code = inner_code, .code_len = 2, .capacity = 2 };
    assert(algorithm_save(txn, inner_algo_id, &inner_pl) == MDB_SUCCESS);

    /* --- Внешний алгоритм: компонует два вызова InnerDerive ---
     * const[0]=CAUSES, const[1]=REF(FIRE), const[2]=REF(SMOKE),
     * const[3]=0(корневая причина), const[4]=inner_algo_id, const[5]=REF(ALARM) */
    Instruction outer_code[] = {
        { .operator_id = OP_LOAD_CONST, .arg = {1, 0} },   /* r1 = CAUSES        */
        { .operator_id = OP_LOAD_CONST, .arg = {2, 1} },   /* r2 = REF(FIRE)      */
        { .operator_id = OP_LOAD_CONST, .arg = {3, 2} },   /* r3 = REF(SMOKE)     */
        { .operator_id = OP_LOAD_CONST, .arg = {4, 3} },   /* r4 = 0 (корень)     */
        { .operator_id = OP_LOAD_CONST, .arg = {7, 4} },   /* r7 = inner_algo_id  */
        { .operator_id = OP_EXEC_ALGORITHM, .arg = {7} },  /* derive FIRE->SMOKE  */
        { .operator_id = OP_MOVE, .arg = {4, 6} },         /* r4 = id предыдущего */
        { .operator_id = OP_LOAD_CONST, .arg = {2, 2} },   /* r2 = REF(SMOKE)     */
        { .operator_id = OP_LOAD_CONST, .arg = {3, 5} },   /* r3 = REF(ALARM)     */
        { .operator_id = OP_EXEC_ALGORITHM, .arg = {7} },  /* derive SMOKE->ALARM */
        { .operator_id = OP_HALT }
    };
    Pipeline outer_pl = { .code = outer_code, .code_len = 11, .capacity = 11 };
    outer_pl.constants.int_consts = malloc(6 * sizeof(int64_t));
    outer_pl.constants.int_consts[0] = (int64_t)CAUSES;
    outer_pl.constants.int_consts[1] = (int64_t)HYPER_MAKE_REF(FIRE);
    outer_pl.constants.int_consts[2] = (int64_t)HYPER_MAKE_REF(SMOKE);
    outer_pl.constants.int_consts[3] = 0;
    outer_pl.constants.int_consts[4] = (int64_t)inner_algo_id;
    outer_pl.constants.int_consts[5] = (int64_t)HYPER_MAKE_REF(ALARM);
    outer_pl.constants.int_count = 6;

    VMContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    WorkingMemory wm;
    memset(&wm, 0, sizeof(wm));
    assert(vm_init(&ctx, txn, &wm) == VM_OK);
    ctx.hyper_mem = hmem;

    int rc = vm_execute(&ctx, &outer_pl);
    assert(rc == VM_OK);

    node_id_t alarm_derive_id = (node_id_t)ctx.reg[6].i;
    assert(alarm_derive_id != 0);

    /* --- Объяснимость: OP_TRACE должен покрыть ОБА составных вызова --- */
    ctx.reg[20].type = REG_INT; ctx.reg[20].i = (int64_t)alarm_derive_id;
    Instruction trace_ins = {0};
    trace_ins.arg[0] = 20; trace_ins.arg[1] = 8; trace_ins.arg[2] = 30; trace_ins.arg[3] = 21;
    assert(vm_op_trace(&ctx, &trace_ins) == VM_OK);

    uint64_t chain_len = (uint64_t)ctx.reg[21].i;
    printf("[Olympiad] длина причинной цепочки = %lu\n", (unsigned long)chain_len);
    assert(chain_len == 2); /* [SMOKE->ALARM derive, FIRE->SMOKE derive] */

    node_id_t alarm_step = (node_id_t)ctx.scratchpad[30].value;
    node_id_t fire_step  = (node_id_t)ctx.scratchpad[31].value;
    assert(alarm_step == alarm_derive_id);
    printf("[Olympiad] trace: %lu -> %lu\n", (unsigned long)alarm_step, (unsigned long)fire_step);

    /* --- Credit assignment должен дойти до FIRE, SMOKE и ALARM --- */
    float before_fire  = score_get(txn, hmem, COGNITIVE_DOMAIN_HYPOTHESIS, FIRE);
    float before_smoke = score_get(txn, hmem, COGNITIVE_DOMAIN_HYPOTHESIS, SMOKE);
    float before_alarm = score_get(txn, hmem, COGNITIVE_DOMAIN_HYPOTHESIS, ALARM);
    assert(before_fire == SCORE_PRIOR && before_smoke == SCORE_PRIOR && before_alarm == SCORE_PRIOR);

    int propagated = score_propagate_credit(txn, hmem, COGNITIVE_DOMAIN_HYPOTHESIS,
                                             alarm_derive_id, 1.0f, 8, 0.6f);
    assert(propagated == 4); /* 2 узла на цепочке * 2 REF-аргумента */

    float after_fire  = score_get(txn, hmem, COGNITIVE_DOMAIN_HYPOTHESIS, FIRE);
    float after_smoke = score_get(txn, hmem, COGNITIVE_DOMAIN_HYPOTHESIS, SMOKE);
    float after_alarm = score_get(txn, hmem, COGNITIVE_DOMAIN_HYPOTHESIS, ALARM);

    printf("[Olympiad] FIRE=%.4f SMOKE=%.4f ALARM=%.4f\n", after_fire, after_smoke, after_alarm);

    assert(after_fire  > before_fire);
    assert(after_smoke > before_smoke);
    assert(after_alarm > before_alarm);
    /* SMOKE и ALARM лежат на более "близком" (менее дисконтированном)
     * шаге SMOKE->ALARM и получают больше веса, чем FIRE, достижимый
     * только через более глубокий и сильнее дисконтированный шаг. */
    assert(after_alarm > after_fire);
    assert(after_smoke > after_fire);

    printf("\n[OK] Композиция алгоритмов (OuterCompose -> InnerDerive x2) сохранила "
           "причинную цепочку и корректно распространила кредит обучения.\n");

    vm_destroy(&ctx);
    hyper_memory_free(hmem);
    free(outer_pl.constants.int_consts);
    mdb_txn_abort(txn);
    close_lmdb();
    system("rm -rf ./test_olympiad_db");
    return 0;
}
