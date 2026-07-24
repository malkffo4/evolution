// runtime/vm/vm_pool.h
#ifndef VM_POOL_H
#define VM_POOL_H

#include "runtime/vm/vm_context.h"

void vm_pool_init(int max_threads);
void vm_pool_submit(Pipeline *pipeline, HyperMemory *hmem, WorkingMemory *wm);
void vm_pool_shutdown(void);

#endif // VM_POOL_H
