#ifndef VM_H
#define VM_H

#include <lmdb.h>

#include "memory/working.h"
#include "runtime/vm/vm_fwd.h"

/* Инициализация и выполнение */
void vm_init(VMContext *vm, MDB_txn *txn, WorkingMemory *wm);
int  vm_execute(VMContext *vm, Pipeline *pipeline);

#endif // VM_H
