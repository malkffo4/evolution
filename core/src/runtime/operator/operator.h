// runtime/operator/operator.h
#ifndef OPERATOR_H
#define OPERATOR_H

#include <stdint.h>

// #include "runtime/vm/vm.h"
#include "runtime/capability/capability.h"
// #include "runtime/executor/executor.h"
#include "runtime/object/object.h"
#include "runtime/operator/operator_types.h"
#include "runtime/vm/vm_fwd.h"
#include "runtime/vm/vm_types.h"

typedef void (*NativeFunction)(
        VMContext *,
        const Instruction *);

typedef struct CompiledCode CompiledCode;

typedef struct {
    OperatorKind kind;

    union {
        NativeFunction native;
        Pipeline *pipeline;
        CompiledCode *compiled;
    };

} OperatorImplementation;

typedef struct Operator {
    OperatorID id;
    const char *name;
    CapabilityMask capability;    // CapabilityID capability;одна возможность (маска), которую реализует оператор / новая связь с планировщиком
    // OperatorKind kind; // MOVE TO OperatorImplementation
    // union {
    //     NativeFunction native;
    //     Pipeline *pipeline;
    //     CompiledCode *compiled;
    // };
    OperatorImplementation impl;
    ObjectType input[8];        // VMObjectType input[8]; чтобы не зависеть от структуры VMObjectType
    int input_count;
    ObjectType output;          // VMObjectType output;
    uint32_t flags;
} Operator;

const Operator *operator_find(OperatorID id);
void operator_register_native(OperatorID id, const char *name, CapabilityMask cap, NativeFunction handler, ObjectType *input, int input_count, ObjectType output);
void operator_register_pipeline(OperatorID id, const char *name, CapabilityMask cap, Pipeline *pipeline, ObjectType *input, int input_count, ObjectType output);
void operator_register_compiled(OperatorID id, const char *name, CapabilityMask cap, void *compiled_code, ObjectType *input, int input_count, ObjectType output);
void operator_registry_init(void);
int operator_register(const Operator *op);
int operator_execute(VMContext *ctx,
                     const Operator *op,
                     const Instruction *ins);

#endif
