// runtime/object/object.h
#pragma once

#include <stdint.h>

#include "runtime/object/object_types.h"
#include "runtime/vm/vm_handle.h"

typedef struct VMObject VMObject;

#define VM_MAX_OBJECTS 1024

enum {
    OBJECT_EDGESET = 1,
    OBJECT_NODESET,
    OBJECT_GRAPH,
    OBJECT_STRING,
    OBJECT_NODE,
    OBJECT_EDGE,
    OBJECT_SCORE
};

typedef struct VMObjectType {
    ObjectType type;
    const char *name;
    void (*destroy)(VMObject *);
    void (*clone)(VMObject *, const VMObject *);
    size_t (*memory_usage)(const VMObject *);
} VMObjectType;

// оператор должен быть полноценным объектом.
// Тогда VM может сама делать: type checking, profiling, logging, statistics, auto documentation
// без знания конкретного оператора.
// Описание в VMObjectType
typedef struct VMObject {
    const VMObjectType *type;
    uint32_t flags;
    uint32_t refcount;
    uint64_t generation;
    void *data;
    VMHandle handle;
} VMObject ;

const VMObjectType *vm_object_type_find(ObjectType type);
