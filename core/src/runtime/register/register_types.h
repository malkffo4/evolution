// runtime/register/register_types.h
#pragma once

/* Типы регистров */
typedef enum {
    REG_EMPTY = 0,
    REG_INT = 1,
    REG_FLOAT = 2,
    REG_BOOL = 3,
    REG_NODE = 4,
    REG_EDGE = 5,
    REG_NODESET = 6,
    REG_EDGESET = 7,
    REG_GRAPH = 8,
    REG_STRING = 9,
    REG_POINTER = 10,
    REG_OBJECT = 11,
    REG_HANDLE = 12
} RegisterType;
