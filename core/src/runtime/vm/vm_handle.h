// runtime/vm/vm_handle.h
#pragma once

#include <stdint.h>

typedef struct VMHandle {
    uint32_t index;
    uint64_t generation;
} VMHandle;

#define VM_INVALID_HANDLE ((VMHandle){ UINT32_MAX, UINT32_MAX })
