// runtime/ops/cognitive/exec_algorithm_by_goal.c
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

#include "runtime/ops/vm_ops.h"
#include "runtime/vm/vm_context.h"
// #include "runtime/vm/vm_status.h"
// #include "runtime/vm/vm.h"
// #include "knowledge/algorithm_loader.h"

int vm_op_exec_algorithm_by_goal(VMContext *ctx, const Instruction *ins) {
    // ... загрузка по goal_id (в будущем) ...
    // Пока что такая же логика, что и exec_algorithm, для совместимости.
    // Но в будущем будет использоваться planner для выбора алгоритма по цели.
    return vm_op_exec_algorithm(ctx, ins);
}
