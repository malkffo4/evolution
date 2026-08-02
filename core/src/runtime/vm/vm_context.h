// runtime/vm/vm_context.h
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "types/id.h"
#include "vm_param.h"
#include "runtime/vm/vm_types.h"
#include "runtime/trace/trace.h"
#include "runtime/arena/arena.h"
#include "runtime/register/register.h"
#include "storage/edge/edge.h"
#include "storage/property/property.h"
#include "storage/hyper_atom/hyper_atom.h"

#define MAX_PRELOADED_EDGES         512
#define MAX_PRELOADED_PROPERTIES    256
#define MAX_SCRATCHPAD              64

typedef struct HyperMemory HyperMemory;

typedef struct {
    node_id_t source;
    node_id_t target;
    node_id_t relation;
} CachedEdge;

typedef struct {
    uint64_t    key_hash;
    int64_t     value;
} ScratchEntry;

typedef struct {
    node_id_t       node_id;
    uint64_t        key_hash;
    PropertyType    type;
    union {
        int64_t i;
        float f;
        bool b;
    } value;
} CachedProperty;

/* Контекст VM */
typedef struct VMContext {
    VMArena         arena;
    VMMemory        memory;
    Register        reg[VM_MAX_REGISTERS];
    VMFrame         frames[VM_MAX_CALL_DEPTH];
    VMProfile       profile[VM_MAX_OPERATORS];
    VMTrace         *trace;
    uint32_t        trace_count;
    uint32_t        trace_capacity;
    uint32_t        frame;          // индекс текущего фрейма
    bool            halted;
    uint64_t        cycles;
    uint64_t        max_cycles;
    VMStatus        status;
    void            *userdata;
    const Operator  *current_operator;
    CachedEdge      preloaded_edges[MAX_PRELOADED_EDGES];
    uint32_t        preloaded_edge_count;
    ScratchEntry    scratchpad[MAX_SCRATCHPAD];
    CachedProperty  preloaded_properties[MAX_PRELOADED_PROPERTIES];
    uint32_t        preloaded_property_count;
    HyperMemory     *hyper_mem;
    ko_id_t         current_context;        // регистры состояний:
    ko_id_t         current_episode_id;
    ko_id_t         last_result_id;         // последний Knowledge Object,
                                            // созданный OP_ASSERT/OP_DERIVE в
                                            // ходе текущего исполнения. Читается
                                            // vm_op_evaluate_goals для Credit
                                            // Assignment после завершения алгоритма.
                                            // Обнуляется вызывающей стороной перед
                                            // запуском и сразу после использования.
    uint32_t        pc;
} VMContext;

// inline обёртки для удобства
static inline VMHandle vm_ctx_object_new(VMContext *ctx, ObjectType type) {
    return vm_object_new(&ctx->arena, type);
}

static inline VMObject *vm_ctx_object_get(VMContext *ctx, VMHandle handle) {
    return vm_object_get(&ctx->arena, handle);
}
