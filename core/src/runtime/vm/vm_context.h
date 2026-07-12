#ifndef VM_CONTEXT
#define VM_CONTEXT

#include <stdint.h>
#include <stdbool.h>

#include "types/id.h"
#include "storage/edge/edge.h"
#include "runtime/arena/arena.h"
#include "vm_param.h"
#include "runtime/register/register.h"
#include "runtime/trace/trace.h"
#include "runtime/vm/vm_types.h"

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
} VMContext;

// inline обёртки для удобства
static inline VMHandle vm_ctx_object_new(VMContext *ctx, ObjectType type) {
    return vm_object_new(&ctx->arena, type);
}

static inline VMObject *vm_ctx_object_get(VMContext *ctx, VMHandle handle) {
    return vm_object_get(&ctx->arena, handle);
}

#endif // VM_CONTEXTVM_CONTEXT
