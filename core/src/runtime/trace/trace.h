// runtime/trace/trace.h
#pragma once

#include <stdint.h>

#include "runtime/compiler/pipline_types.h"
#include "runtime/register/register_types.h"
#include "runtime/vm/vm_fwd.h"
#include "runtime/vm/vm_types.h"
#include "runtime/vm/vm_status.h"

#define VM_TRACE_INITIAL_CAPACITY 1024

typedef enum {
    TRACE_SUCCESS = 1<<0,
    TRACE_WARNING = 1<<1,
    TRACE_TIMEOUT = 1<<2,
    TRACE_FALLBACK = 1<<3,
    TRACE_EXCEPTION = 1<<4

} VMTraceFlags;

typedef struct VMTraceRegister {
    RegisterType type;
    uint32_t value;
} VMTraceRegister;

typedef struct VMTrace {
    OperatorID operator;
    PipelineID pipeline;

    uint64_t cycles;
    uint64_t begin;
    uint64_t end;

    uint32_t depth;
    uint32_t ip;

    uint32_t flags;

    VMTraceRegister input[4];
    VMTraceRegister output[2];

    VMStatus result;
} VMTrace;

int vm_trace_init(VMContext *ctx);

void vm_trace_destroy(VMContext *ctx);

VMTrace *vm_trace_begin(VMContext *ctx, OperatorID id);

void vm_trace_end(VMContext *ctx, VMTrace *trace, VMStatus result);
