// runtime/arena.c

#include <stdlib.h>
#include <string.h>

#include "runtime/object/object.h"
#include "runtime/object/object_types.h"
#include "runtime/arena/arena.h"

static int vm_arena_grow(VMArena *arena) {
    uint32_t old = arena->capacity;
    uint32_t cap = old ? old * 2 : VM_ARENA_INITIAL_CAPACITY;

    VMObject *objects = realloc(arena->objects, cap * sizeof(VMObject));

    if (!objects)
        return 0;

    uint32_t *stack = realloc(arena->free_stack, cap * sizeof(uint32_t));

    if (!stack) {
        free(objects);
        // arena->objects = objects;
        return 0;
    }

    arena->objects = objects;
    arena->free_stack = stack;

    memset(arena->objects + old, 0, (cap - old) * sizeof(VMObject));

    for (uint32_t i = old; i < cap; i++)
        arena->free_stack[arena->free_count++] = cap - 1 - (i - old);

    arena->capacity = cap;

    return 1;
}

VMHandle vm_object_new(VMArena *arena, ObjectType type) {
    if (!arena->free_count) {
        if (!vm_arena_grow(arena))
            return VM_INVALID_HANDLE;
    }

    uint32_t id = arena->free_stack[--arena->free_count];
    VMObject *obj = &((VMObject*)arena->objects)[id];

    memset(obj, 0, sizeof(*obj));

    obj->generation++;
    obj->refcount = 1;
    obj->type = vm_object_type_find(type);
    obj->handle.index = id;
    obj->handle.generation = obj->generation;

    if(!obj->type)
        return VM_INVALID_HANDLE;

    return (VMHandle){ .index = id, .generation = obj->generation };
}

VMObject *vm_object_get(VMArena *arena, VMHandle handle) {
    if (handle.index >= arena->capacity)   // ← arena->, не arena.
        return NULL;

    VMObject *obj = &arena->objects[handle.index];  // ← arena->, не arena.

    if (obj->generation != handle.generation)
        return NULL;

    if (!obj->refcount)
        return NULL;

    return obj;
}

void vm_object_retain(VMArena *arena, VMHandle handle) {
    VMObject *obj = vm_object_get(arena, handle);  // ← arena, не ctx

    if (!obj)
        return;

    if(obj->refcount == UINT32_MAX)
        return;

    obj->refcount++;
}

void vm_object_release(VMArena *arena, VMHandle handle) {
    VMObject *obj = vm_object_get(arena, handle);  // ← arena, не ctx

    if (!obj)
        return;

    if (--obj->refcount)
        return;

    const VMObjectType *type = vm_object_type_find(obj->type->type);  // ← type->type, не type.type

    if (type && type->destroy)
        type->destroy(obj);

    obj->generation++;

    memset(obj, 0, sizeof(*obj));

    arena->free_stack[arena->free_count++] = handle.index;  // ← arena->, не ctx->
}
