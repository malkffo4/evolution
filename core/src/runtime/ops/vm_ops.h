#ifndef VM_OPS_H
#define VM_OPS_H

#include "runtime/vm/vm_context.h"

// typedef int (*VMOperatorFn)(VMContext *ctx, const Instruction *ins);

int vm_op_nop(VMContext *,  const Instruction *);

int vm_op_load_const(VMContext *, const Instruction *);

int vm_op_move(VMContext *, const Instruction *);

int vm_op_add(VMContext *, const Instruction *);

int vm_op_store(VMContext *, const Instruction *);

int vm_op_clear(VMContext *, const Instruction *);

// int vm_op_get_in_edges(VMContext *, const Instruction *);

// int vm_op_get_out_edges(VMContext *, const Instruction *);

// int vm_op_match_greedy(VMContext *, const Instruction *);

// int vm_op_score(VMContext *, const Instruction *);

int vm_op_branch(VMContext *, const Instruction *);

int vm_op_branch_if_empty(VMContext *, const Instruction *);

int vm_op_call(VMContext *, const Instruction *);

int vm_op_return(VMContext *, const Instruction *);

int vm_op_halt(VMContext *, const Instruction *);

#endif
