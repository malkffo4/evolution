// runtime/ops/vm_ops.h
#ifndef VM_OPS_H
#define VM_OPS_H

#include "runtime/vm/vm_context.h"

// typedef int (*VMOperatorFn)(VMContext *ctx, const Instruction *ins);

int vm_op_nop(VMContext *,  const Instruction *);


int vm_op_add(VMContext *, const Instruction *);

int vm_op_clear(VMContext *, const Instruction *);

int vm_op_load_const(VMContext *, const Instruction *);

int vm_op_move(VMContext *, const Instruction *);

int vm_op_set_tmp(VMContext *, const Instruction *);

int vm_op_store(VMContext *, const Instruction *);


// int vm_op_get_in_edges(VMContext *, const Instruction *);

// int vm_op_get_out_edges(VMContext *, const Instruction *);

// int vm_op_match_greedy(VMContext *, const Instruction *);

// int vm_op_score(VMContext *, const Instruction *);

int vm_op_branch(VMContext *, const Instruction *);

int vm_op_branch_if_empty(VMContext *, const Instruction *);

int vm_op_call(VMContext *, const Instruction *);

int vm_op_halt(VMContext *, const Instruction *);

int vm_op_return(VMContext *, const Instruction *);


int vm_op_check_cached_edge(VMContext *, const Instruction *);

int vm_op_prop_set(VMContext *, const Instruction *);

int vm_op_prop_get(VMContext *, const Instruction *);

int vm_op_concat_paths(VMContext *, const Instruction *);

int vm_op_exec_algorithm(VMContext *, const Instruction *);

int vm_op_find_similar(VMContext *, const Instruction *);

int vm_op_get_neighbors(VMContext *, const Instruction *);

int vm_op_read_sp(VMContext *, const Instruction *);

int vm_op_query(VMContext *, const Instruction *);
int vm_op_assert(VMContext *, const Instruction *);
int vm_op_derive(VMContext *, const Instruction *);
int vm_op_trace(VMContext *, const Instruction *);
int vm_op_spawn_ctx(VMContext *, const Instruction *);
int vm_op_merge_ctx(VMContext *, const Instruction *);

// Заглушки (пока просто вызывают старые функции)
int vm_op_spread_activation(VMContext *ctx, const Instruction *ins);
int vm_op_evaluate_goals(VMContext *ctx, const Instruction *ins);

int vm_op_load_context(VMContext *ctx, const Instruction *ins);

int vm_op_vector_sim(VMContext *ctx, const Instruction *ins);

#endif
