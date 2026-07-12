#ifndef ARENA_H
#define ARENA_H

#include <stdint.h>
// #include "runtime/vm/vm.h"
#include "runtime/vm/vm_fwd.h"
#include "runtime/object/object_types.h"

typedef struct VMHandle {
    uint32_t index;
    uint64_t generation;
} VMHandle;

#define VM_INVALID_HANDLE ((VMHandle){ UINT32_MAX, UINT32_MAX })

#define VM_MAX_OBJECTS 1024

// GetEdges -> создает ArenaObject
// ↓
// регистр хранит handle
// Это намного быстрее.
#define VM_ARENA_INITIAL_CAPACITY 256

typedef struct VMArena {
    VMObject *objects;
    uint32_t *free_stack;
    // uint32_t count; = capacity - free_count
    uint32_t capacity;
    uint32_t free_count;
} VMArena;

// VMHandle
// VMArena
// vm_object_new()
// vm_object_get()
// vm_object_retain()
// vm_object_release()
// vm_arena_init()
// vm_arena_destroy()
// TODO
VMHandle vm_object_new(VMContext *ctx, ObjectType type);
VMObject *vm_object_get(VMContext *ctx, VMHandle handle);

void vm_object_retain(VMContext *ctx, VMHandle handle);

void vm_object_release(VMContext *ctx, VMHandle handle);

#endif // ARENA_H
