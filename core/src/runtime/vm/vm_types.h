// runtime/vm/vm_types.h
#ifndef VM_TYPES_H
#define VM_TYPES_H

#include <lmdb.h>

#include "runtime/vm/vm_fwd.h"
#include "runtime/arena/arena.h"
#include "memory/working.h"
#include "runtime/operator/operator_types.h"

/* Фрейм вызова */
typedef struct VMFrame {
    const Pipeline *pipeline;
    Instruction *code;
    uint32_t ip;
    OperatorID caller; // caller_frame Потом Trace сможет строить call graph.
    uint16_t return_reg;
    uint32_t return_ip;
} VMFrame;

// Профилирование
// ctx->profile[opcode].calls++;
// ctx->profile[opcode].
// Потом ИИ сам увидит Capability, MatchEdges = 100000 вызовов, 74% времени и сможет заменить его.
typedef struct VMProfile {
    uint64_t calls;  // calls++;
    uint64_t cycles; // cycles += cycles;

    uint64_t min_cycles;
    uint64_t max_cycles;

    uint64_t failures;
} VMProfile;

typedef struct VMMemory {
    VMArena arena;          // временные объекты текущего запуска
    WorkingMemory *wm;      // долговременные объекты между пайплайнами
    MDB_txn *txn;           // постоянное хранилище
} VMMemory;

/* Сигнатура обработчика инструкции */
typedef int (*VMHandler)(VMContext *ctx, const Instruction *ins);

#endif // VM_TYPES_H
