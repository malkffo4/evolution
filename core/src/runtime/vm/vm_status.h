// runtime/vm/vm_status.h
#ifndef VM_STATUS_H
#define VM_STATUS_H

/* Статусы выполнения */
typedef enum {
    VM_OK = 0,
    VM_ERROR,
    VM_UNKNOWN_OPCODE,
    VM_INVALID_REGISTER,
    VM_INVALID_TYPE,
    VM_STACK_OVERFLOW,
    VM_STACK_UNDERFLOW,
    VM_TIMEOUT,
    VM_OUT_OF_MEMORY
} VMStatus;

#endif // VM_STATUS_H
