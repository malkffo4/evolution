// runtime/arena.c

#include <stdlib.h>
#include <string.h>

#include "runtime/arena/arena.h"
#include "runtime/vm/vm.h"

static int vm_arena_grow(VMArena *arena) {
    uint32_t old = arena->capacity;
    uint32_t cap = old ? old * 2 : VM_ARENA_INITIAL_CAPACITY;

    VMObject *objects =
        realloc(arena->objects,
            cap * sizeof(VMObject));

    if (!objects)
        return 0;

    uint32_t *stack =
        realloc(arena->free_stack,
            cap * sizeof(uint32_t));

    if (!stack)
    {
        arena->objects = objects;
        return 0;
    }

    arena->objects = objects;
    arena->free_stack = stack;

    memset(arena->objects + old,
           0,
           (cap - old) * sizeof(VMObject));

    for (uint32_t i = old; i < cap; i++)
        arena->free_stack[arena->free_count++] = cap - 1 - (i - old);

    arena->capacity = cap;

    return 1;
}

VMHandle vm_object_new(VMContext *ctx, ObjectType type) {
    VMArena *arena = &ctx->arena;

    if (!arena->free_count) {
        if (!vm_arena_grow(arena))
            return VM_INVALID_HANDLE;
    }

    uint32_t id =
        arena->free_stack[--arena->free_count];

    VMObject *obj =
        &arena->objects[id];

    memset(obj,0,sizeof(*obj));

    obj->generation++;
    obj->refcount = 1;
    obj->type = vm_object_type_find(type);
    if(!obj->type)
        return VM_INVALID_HANDLE;

    return (VMHandle)
    {
        .index = id,
        .generation = obj->generation
    };
}

VMObject *vm_object_get(VMContext *ctx, VMHandle handle) {
    if (handle.index >= ctx->arena.capacity)
        return NULL;

    VMObject *obj =
        &ctx->arena.objects[handle.index];

    if (obj->generation != handle.generation)
        return NULL;

    if (!obj->refcount)
        return NULL;

    return obj;
}

void vm_object_retain(VMContext *ctx, VMHandle handle) {
    VMObject *obj =
        vm_object_get(ctx, handle);

    if (!obj)
        return;

    if(obj->refcount==UINT32_MAX)
        return;

    obj->refcount++;
}

void vm_object_release(VMContext *ctx, VMHandle handle) {
    VMObject *obj =
        vm_object_get(ctx, handle);

    if (!obj)
        return;

    if (--obj->refcount)
        return;

    const VMObjectType *type =
        vm_object_type_find(obj->type);

    if (type && type->destroy)
        type->destroy(obj);

    obj->generation++;

    memset(obj,0,sizeof(*obj));

    ctx->arena.free_stack[
        ctx->arena.free_count++
    ] = handle.index;
}
