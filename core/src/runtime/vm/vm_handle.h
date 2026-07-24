// runtime/vm/vm_handle.h
#ifndef VM_HANDLE_H
#define VM_HANDLE_H

#include <stdint.h>

typedef struct VMHandle {
    uint32_t index;
    uint64_t generation;
} VMHandle;

#define VM_INVALID_HANDLE ((VMHandle){ UINT32_MAX, UINT32_MAX })

#endif // VM_HANDLE_H
