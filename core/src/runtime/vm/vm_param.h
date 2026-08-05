// runtime/vm/vm_param.h
#pragma once

/* Максимумы */
#define VM_MAX_REGISTERS    64
#define VM_MAX_CALL_DEPTH   32
#define VM_MAX_CYCLES       1000000
#define VM_MAX_OPERATORS    4096
#define VM_EVAL_GRAPH_DEFAULT_STEPS 256   // защита от бесконечного/циклического графа
