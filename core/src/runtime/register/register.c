#include <string.h>

#include "runtime/vm/vm_fwd.h"
#include "runtime/arena/arena.h"

VMObject *vm_register_object(VMContext *ctx, const Register *reg) {
    if (reg->type != REG_HANDLE)
        return NULL;

    return vm_object_get(ctx, reg->handle);
}

void vm_register_clear(VMContext *ctx, Register *reg) {
    if (reg->type == REG_OBJECT) {
        VMObject *obj = vm_object_get(ctx, reg->handle);
        if (obj) vm_object_release(ctx, reg->handle);
    }
    memset(reg, 0, sizeof(*reg));
}

void vm_register_move(VMContext *ctx, Register *dst, Register *src) {
    vm_register_clear(ctx, dst);
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

void vm_register_copy(VMContext *ctx, Register *dst, const Register *src) {
    vm_register_clear(ctx, dst);
    *dst = *src;
    if (src->type == REG_OBJECT) {
        VMObject *obj = vm_object_get(ctx, src->handle);
        if (obj) vm_object_retain(ctx, src->handle);
    }
}

void vm_register_set_int(VMContext *ctx, Register *reg, int64_t value) {
    vm_register_clear(ctx, reg);
    reg->type = REG_INT;
    reg->i = value;
}

void vm_register_set_float(VMContext *ctx, Register *reg, double value) {
    vm_register_clear(ctx, reg);
    reg->type = REG_FLOAT;
    reg->f = value;
}

void vm_register_set_bool(VMContext *ctx, Register *reg, bool value) {
    vm_register_clear(ctx, reg);
    reg->type = REG_BOOL;
    reg->b = value;
}

void vm_register_set_node(VMContext *ctx, Register *reg, node_id_t node) {
    vm_register_clear(ctx, reg);
    reg->type = REG_NODE;
    reg->node = node;
}

void vm_register_set_handle(VMContext *ctx, Register *reg, VMObject *obj) {
    vm_register_clear(ctx, reg);
    if (obj) {
        vm_object_retain(ctx, obj->handle); // объект передан по указателю? нужно уточнить
        reg->type = REG_OBJECT;
        reg->handle = obj->handle;
    }
}

VMObject *vm_register_get_object(VMContext *ctx, Register *reg) {
    if (reg->type != REG_OBJECT) return NULL;
    return vm_object_get(ctx, reg->handle);
}

void vm_register_swap(VMContext *ctx, Register *a, Register *b) {
    Register tmp;
    memcpy(&tmp, a, sizeof(Register));
    memcpy(a, b, sizeof(Register));
    memcpy(b, &tmp, sizeof(Register));
}


VMHandler vm_handlers[VM_OPCODE_COUNT];

void vm_registry_init(void) {
    vm_handlers[OP_NOP]            = vm_op_nop;
    vm_handlers[OP_LOAD_CONST]     = vm_op_load_const;
    vm_handlers[OP_MOVE]           = vm_op_move;
    vm_handlers[OP_STORE]          = vm_op_store;
    vm_handlers[OP_CLEAR]          = vm_op_clear;
    vm_handlers[OP_GET_IN_EDGES]   = vm_op_get_in_edges;
    vm_handlers[OP_GET_OUT_EDGES]  = vm_op_get_out_edges;
    vm_handlers[OP_MATCH_GREEDY]   = vm_op_match_greedy;
    vm_handlers[OP_SCORE]          = vm_op_score;
    vm_handlers[OP_BRANCH]         = vm_op_branch;
    vm_handlers[OP_BRANCH_IF_EMPTY]= vm_op_branch_if_empty;
    vm_handlers[OP_CALL]           = vm_op_call;
    vm_handlers[OP_RETURN]         = vm_op_return;
    vm_handlers[OP_HALT]           = vm_op_halt;
    // Остальные (FILTER, SORT, INTERSECTION...) пока можно оставить NULL
}
