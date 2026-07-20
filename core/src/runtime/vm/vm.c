// runtime/vm/vm.c
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/vm/vm_status.h"
#include "runtime/vm/vm_context.h"
#include "runtime/trace/trace.h"
#include "runtime/ops/opcode.h"
#include "runtime/planner.h"
#include "runtime/operator/operator.h"

/* регистры не инициализируем – будут заполнены операциями */
int vm_init(VMContext *ctx, MDB_txn *txn, WorkingMemory *wm) {
    memset(ctx, 0, sizeof(VMContext));
    ctx->memory.txn = txn;
    ctx->memory.wm = wm;
    ctx->frame = 0;
    ctx->halted = false;
    ctx->cycles = 0;
    ctx->max_cycles = VM_MAX_CYCLES;
    ctx->status = VM_OK;

    // Инициализация трассировки
    if (!vm_trace_init(ctx)) {
        return VM_ERROR; // если не удалось выделить память
    }

    // ARENA
    ctx->arena.capacity = VM_MAX_OBJECTS;

    ctx->arena.objects = calloc(ctx->arena.capacity, sizeof(VMObject));
    if (!ctx->arena.objects) {
        // LOG?
        return VM_ERROR;
    }

    ctx->arena.free_stack = malloc(ctx->arena.capacity * sizeof(uint32_t));
    if (!ctx->arena.free_stack) {
        // LOG??
        return VM_ERROR;
    }

    ctx->arena.free_count = VM_MAX_OBJECTS;

    for(uint32_t i=0;i<VM_MAX_OBJECTS;i++)
        ctx->arena.free_stack[i]=VM_MAX_OBJECTS-i-1;

    return VM_OK;
}

void vm_destroy(VMContext *ctx) {
    vm_trace_destroy(ctx);

    for(uint32_t i=0;i<ctx->arena.capacity;i++) {
        VMObject *obj=&ctx->arena.objects[i];

        if(!obj->refcount)
            continue;

        // const VMObjectType *type = vm_object_type_find(obj->type);
        // const VMObjectType *type = obj->type;
        // if(type && type->destroy)
        //     type->destroy(obj);
        if (obj->type && obj->type->destroy)
            obj->type->destroy(obj);
    }

    free(ctx->arena.objects);
    free(ctx->arena.free_stack);
}

int vm_execute(VMContext *ctx, const Pipeline *pipeline) {
    ctx->frame = 0;
    ctx->halted = false;
    ctx->cycles = 0;

    ctx->frames[0].pipeline = pipeline;
    ctx->frames[0].code = pipeline->code;
    ctx->frames[0].ip = 0;

    for(;;) {
        if(ctx->halted)
            break;

        if(ctx->cycles>=ctx->max_cycles)
            return VM_TIMEOUT;

        VMFrame *frame=&ctx->frames[ctx->frame];

        if(frame->ip>=frame->pipeline->code_len)
            break;

        const Instruction *ins = &frame->code[frame->ip++];

        const Operator *op;
        if (ins->operator_id != 0) {
            op = operator_find(ins->operator_id);
        } else {
            op = planner_choose(ctx, ins->capability_mask);
        }

        if (!op)
            return VM_UNKNOWN_OPCODE; // VM_UNKNOWN_OPERATOR

        ctx->current_operator = op;

        VMTrace *trace = vm_trace_begin(ctx, op->id);

        int rc = operator_execute(ctx, op, ins);

        vm_trace_end(ctx, trace, rc);

        uint64_t dt = trace->end - trace->begin;

        VMProfile *p = &ctx->profile[op->id];

        p->calls++;
        p->cycles += dt;

        if(!p->min_cycles || dt < p->min_cycles)
            p->min_cycles = dt;

        if(dt > p->max_cycles)
            p->max_cycles = dt;

        if(rc != VM_OK)
            p->failures++;
    }

    return VM_OK;
}
