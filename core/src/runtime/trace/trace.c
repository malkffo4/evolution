// runtime/trace/trace.c
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/time/time.h"
#include "runtime/trace/trace.h"
#include "runtime/vm/vm_context.h"

static int vm_trace_grow(VMContext *ctx) {
    uint32_t cap =
        ctx->trace_capacity ?
        ctx->trace_capacity * 2 :
        VM_TRACE_INITIAL_CAPACITY;

    VMTrace *trace = realloc(ctx->trace, cap * sizeof(VMTrace));

    if(!trace)
        return 0;

    ctx->trace = trace;
    ctx->trace_capacity = cap;

    return 1;
}

int vm_trace_init(VMContext *ctx) {
    ctx->trace = NULL;
    ctx->trace_count = 0;
    ctx->trace_capacity = 0;

    return vm_trace_grow(ctx);
}

void vm_trace_destroy(VMContext *ctx) {
    free(ctx->trace);

    ctx->trace = NULL;
    ctx->trace_count = 0;
    ctx->trace_capacity = 0;
}

VMTrace *vm_trace_begin(VMContext *ctx, OperatorID id) {
    if(ctx->trace_count >= ctx->trace_capacity) {
        if(!vm_trace_grow(ctx))
            return NULL;
    }

    VMTrace *t = &ctx->trace[ctx->trace_count++];

    memset(t,0,sizeof(*t));

    t->operator = id;
    t->begin = vm_rdtsc();
    t->depth = ctx->frame;

    return t;
}

void vm_trace_end(VMContext *ctx, VMTrace *trace, VMStatus result) {
    (void)ctx;

    if(!trace)
        return;

    trace->end = vm_rdtsc();
    trace->result = result;
}
