#ifndef ARENA_H
#define ARENA_H

#include <stdint.h>
#include "runtime/vm/vm_fwd.h"
#include "runtime/vm/vm_handle.h"
#include "runtime/object/object_types.h"

// Forward declaration
// typedef struct VMObject VMObject;

#define VM_ARENA_INITIAL_CAPACITY 256

typedef struct VMArena {
    VMObject *objects;      // временно void*, позже
    uint32_t *free_stack;
    uint32_t capacity;
    uint32_t free_count;
} VMArena;

// Теперь принимают VMArena*, а не VMContext*
VMHandle vm_object_new(VMArena *arena, ObjectType type);
VMObject *vm_object_get(VMArena *arena, VMHandle handle);
void vm_object_retain(VMArena *arena, VMHandle handle);
void vm_object_release(VMArena *arena, VMHandle handle);

#endif // ARENA_H
