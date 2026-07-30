// runtime/operator/operator.c
#include <string.h>

#include "runtime/operator/operator.h"
#include "runtime/planner.h"
#include "runtime/ops/opcode.h"
#include "runtime/ops/vm_ops.h"
#include "runtime/logging/logging.h"

#define MAX_OPERATORS 4096
static Operator operator_table[MAX_OPERATORS];
static bool operator_used[MAX_OPERATORS];

// ----- исполнители -----
// static int executor_native_execute(VMContext *ctx, const Operator *op, const Instruction *ins) {
//     return op->native_func(ctx, ins);
// }
// static int executor_pipeline_execute(VMContext *ctx, const Operator *op, const Instruction *ins) {
//     if (ctx->frame + 1 >= VM_MAX_CALL_DEPTH) return VM_STACK_OVERFLOW;
//     ctx->frames[ctx->frame].ip = ctx->frames[ctx->frame].ip;
//     ctx->frame++;
//     VMFrame *f = &ctx->frames[ctx->frame];
//     f->pipeline = op->pipeline;
//     f->code = f->pipeline->code;
//     f->ip = 0;
//     f->caller = op->id;
//     return VM_OK;
// }
static int compiled_execute(VMContext *ctx, CompiledCode *compiled, const Instruction *ins) {
    (void)ctx;
    (void)compiled;
    (void)ins;

    /* TODO:
     * JIT
     * WASM
     * LLVM
     * Neural backend
     */

    return VM_ERROR;
}

// ----- регистрация -----
int operator_register(const Operator *op) {
    if (op->id >= MAX_OPERATORS)
        return -1;

    operator_table[op->id] = *op;
    operator_used[op->id] = true;

    planner_register_capability(&operator_table[op->id], op->capability);

    return 0;
}

const Operator *operator_find(OperatorID id) {
    if (id >= MAX_OPERATORS)
        return NULL;

    if (!operator_used[id])
        return NULL;

    return &operator_table[id];
}

OperatorID operator_find_by_name(const char *name) {
    for (uint32_t i = 0; i < MAX_OPERATORS; i++) {
        if (operator_used[i] && strcmp(operator_table[i].name, name) == 0)
            return operator_table[i].id;
    }
    return 0;
}

void operator_register_native(OperatorID id, const char *name, CapabilityMask cap,
                              NativeFunction handler, ObjectType *input, int input_count, ObjectType output) {
    Operator op = {
        .id = id, .name = name, .capability = cap,
        .impl.kind = OPERATOR_NATIVE, .impl.native = handler,
        .input_count = input_count, .output = output, .flags = 0
    };
    if (input && input_count)
        memcpy(op.input, input, (size_t)input_count * sizeof(ObjectType));

    operator_register(&op);
}

void operator_register_pipeline(OperatorID id, const char *name, CapabilityMask cap,
                                Pipeline *pipeline, ObjectType *input, int input_count, ObjectType output) {
    Operator op = {
        .id = id, .name = name, .capability = cap,
        .impl.kind = OPERATOR_PIPELINE, .impl.pipeline = pipeline,
        .input_count = input_count, .output = output, .flags = 0
    };
    if (input && input_count)
        memcpy(op.input, input, (size_t)input_count * sizeof(ObjectType));

    operator_register(&op);
}

void operator_register_compiled(OperatorID id, const char *name, CapabilityMask cap,
                                void *compiled_code, ObjectType *input, int input_count, ObjectType output) {
    Operator op = {
        .id = id, .name = name, .capability = cap,
        .impl.kind = OPERATOR_COMPILED, .impl.compiled = compiled_code,
        .input_count = input_count, .output = output, .flags = 0
    };
    if (input && input_count)
        memcpy(op.input, input, (size_t)input_count * sizeof(ObjectType));

    operator_register(&op);
}

// ----- начальная загрузка -----
void operator_registry_init(void) {
    planner_init_default_policy();   // политика по умолчанию

    // 1. Управляющие операторы (capability = 0)
    operator_register_native(OP_NOP,   "nop",    0, vm_op_nop,   NULL, 0, 0);
    operator_register_native(OP_HALT,  "halt",   0, vm_op_halt,  NULL, 0, 0);
    operator_register_native(OP_CALL,  "call",   0, vm_op_call,  NULL, 0, 0);
    operator_register_native(OP_RETURN,"return", 0, vm_op_return,NULL, 0, 0);
    operator_register_native(OP_LOAD_CONST, "load_const", 0, vm_op_load_const, NULL, 0, 0);
    operator_register_native(OP_MOVE,  "move",   0, vm_op_move,  NULL, 0, 0);
    operator_register_native(OP_SUB, "sub", 0, vm_op_sub, NULL, 0, 0);
    operator_register_native(OP_MUL, "mul", 0, vm_op_mul, NULL, 0, 0);
    operator_register_native(OP_DIV, "div", 0, vm_op_div, NULL, 0, 0);
    operator_register_native(OP_ADD,  "add",   0, vm_op_add,  NULL, 0, 0);
    operator_register_native(OP_CHECK_CACHED_EDGE, "check_cached_edge", 0, vm_op_check_cached_edge, NULL, 0, 0);
    operator_register_native(OP_EXEC_ALGORITHM, "exec_algorithm", 0, vm_op_exec_algorithm, NULL, 0, 0);
    operator_register_native(OP_GET_NEIGHBORS, "get_neighbors", 0, vm_op_get_neighbors, NULL, 0, 0);
    operator_register_native(OP_FIND_SIMILAR,  "find_similar",  0, vm_op_find_similar,  NULL, 0, 0);
    operator_register_native(OP_READ_SP,       "read_sp",       0, vm_op_read_sp,       NULL, 0, 0);
    operator_register_native(OP_CONCAT_PATHS,  "concat_paths",  0, vm_op_concat_paths,  NULL, 0, 0);
    operator_register_native(OP_PROP_GET,  "get_property",  0, vm_op_prop_get,  NULL, 0, 0);
    operator_register_native(OP_PROP_SET,  "set_property",  0, vm_op_prop_set,  NULL, 0, 0);
    operator_register_native(OP_SET_TMP, "set_tmp", 0, vm_op_set_tmp, NULL, 0, 0);

    operator_register_native(OP_QUERY,       "query",        0, vm_op_query,       NULL, 0, 0);
    operator_register_native(OP_ASSERT,      "assert",       0, vm_op_assert,      NULL, 0, 0);
    operator_register_native(OP_DERIVE,      "derive",       0, vm_op_derive,      NULL, 0, 0);
    operator_register_native(OP_TRACE,       "trace",        0, vm_op_trace,       NULL, 0, 0);
    operator_register_native(OP_SPAWN_CTX,   "spawn_ctx",    0, vm_op_spawn_ctx,   NULL, 0, 0);
    operator_register_native(OP_MERGE_CTX,   "merge_ctx",    0, vm_op_merge_ctx,   NULL, 0, 0);

    operator_register_native(OP_SPREAD_ACTIVATION, "spread_activation", 0, vm_op_spread_activation, NULL, 0, 0);
    operator_register_native(OP_EVALUATE_GOALS,    "evaluate_goals",    0, vm_op_evaluate_goals,    NULL, 0, 0);

    operator_register_native(OP_LOAD_CONTEXT, "load_context", 0, vm_op_load_context, NULL, 0, 0);

    operator_register_native(OP_VECTOR_SIM, "vector_sim", 0, vm_op_vector_sim, NULL, 0, 0);
    operator_register_native(OP_CRITIC_APPLY, "critic_apply", 0, vm_op_critic_apply, NULL, 0, 0);

    operator_register_native(OP_MATCH_PATTERN, "match_pattern", 0, vm_op_match_pattern, NULL, 0, 0);
    operator_register_native(OP_CREDIT_ASSIGN, "credit_assign", 0, vm_op_credit_assign, NULL, 0, 0);

    // 2. Операторы с возможностями
    // ObjectType in_node[] = { REG_NODE };
    // operator_register_native(OP_GET_OUT_EDGES, "get_out_edges", CAP_GET_OUT_EDGES, vm_op_get_out_edges, in_node, 1, REG_EDGESET);
    // альтернативные реализации той же возможности
    // operator_register_native(OP_GET_OUT_EDGES_FAST, "get_out_edges_fast", CAP_GET_OUT_EDGES, vm_op_get_out_edges, in_node, 1, REG_EDGESET);   // пока тот же обработчик
    // в будущем:
    // operator_register_compiled(OP_GET_OUT_EDGES_NEURAL, "get_out_edges_neural", CAP_GET_OUT_EDGES,
    //                            neural_model, in_node, 1, REG_EDGESET);
}

// TODO ?
// FastFindEdges
// GpuFindEdges
// ApproximateFindEdges
// DistributedFindEdges

int operator_execute(VMContext *ctx, const Operator *op, const Instruction *ins) {
    switch (op->impl.kind) {
        case OPERATOR_NATIVE:
            return op->impl.native(ctx, ins);

        case OPERATOR_PIPELINE: {
            if (ctx->frame + 1 >= VM_MAX_CALL_DEPTH) {
                LOG_ERROR("VM: Stack Overflow! Max depth %d reached.", VM_MAX_CALL_DEPTH);
                return VM_STACK_OVERFLOW;
            }

            VMFrame *f = &ctx->frames[++ctx->frame];

            f->pipeline = op->impl.pipeline;
            f->code     = op->impl.pipeline->code;
            f->ip       = 0;
            f->caller   = op->id;

            LOG_DEBUG("VM: PUSH FRAME -> Entered pipeline %s (Depth: %d)", op->name, ctx->frame);

            return VM_OK;
        }

        case OPERATOR_COMPILED:
            return compiled_execute(ctx, op->impl.compiled, ins);

        default:
            return VM_ERROR;
    }
}
