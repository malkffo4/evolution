// runtime/vm/vm.h
#pragma once

#include <lmdb.h>

#include "memory/working.h"
#include "runtime/vm/vm_fwd.h"

/* Инициализация и выполнение */
int  vm_init(VMContext *vm, MDB_txn *txn, WorkingMemory *wm);
int  vm_execute(VMContext *vm, Pipeline *pipeline);
void vm_destroy(VMContext *ctx);
