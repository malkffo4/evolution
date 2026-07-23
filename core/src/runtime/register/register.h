#ifndef VM_REGISTER_H
#define VM_REGISTER_H

#include <stdint.h>
#include <stdbool.h>

#include "runtime/vm/vm_types.h"
#include "runtime/vm/vm_fwd.h"
#include "types/id.h"
// #include "runtime/operator/operator.h"
// #include "runtime/ops/vm_ops.h"
#include "runtime/compiler/pipeline.h"
#include "runtime/register/register_types.h"

// EdgeList, NodeList,GraphView, Edge, MatchResult, Score, Analogy - ВСЕ живут только в Arena.
/* Один регистр */
typedef struct Register {
    RegisterType    type;
    union {
        int64_t     i;
        uint64_t    ui;
        double      f;
        bool        b;
        node_id_t   node;
        VMHandle    handle;
        StringView  string;
        void        *ptr;
    };
} Register;

void vm_registry_init(void);
// vm_register()
// vm_unregister()
// vm_find()
// тогда потом можно будет
// load operator
// learn operator
// replace operator
// без перекомпиляции.

VMObject *vm_register_object(VMContext *ctx, const Register *reg);

void vm_register_clear(VMContext *ctx, Register *reg);

void vm_register_clear(VMContext *ctx, Register *reg);

void vm_register_swap(VMContext *ctx, Register *a, Register *b);

VMObject *vm_register_get_object(VMContext *ctx, Register *reg);

void vm_register_set_handle(VMContext *ctx, Register *reg, VMObject *obj);

void vm_register_set_node(VMContext *ctx, Register *reg, node_id_t node);

void vm_register_set_float(VMContext *ctx, Register *reg, double value);

void vm_register_set_bool(VMContext *ctx, Register *reg, bool value);

void vm_register_set_int(VMContext *ctx, Register *reg, int64_t value);

void vm_register_copy(VMContext *ctx, Register *dst, const Register *src);

void vm_register_move(VMContext *ctx, Register *dst, Register *src);

int64_t vm_register_read_int(VMContext *ctx, uint32_t reg_id);

#endif // VM_REGISTER_H
