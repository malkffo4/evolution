// runtime/register/register_types.h
#ifndef REGISTER_TYPES_H
#define REGISTER_TYPES_H

/* Типы регистров */
typedef enum {
    REG_EMPTY,
    REG_INT,
    REG_FLOAT,
    REG_BOOL,
    REG_NODE,
    REG_EDGE,
    REG_NODESET,
    REG_EDGESET,
    REG_GRAPH,
    REG_STRING,
    REG_POINTER,
    REG_OBJECT
} RegisterType;

#endif // REGISTER_TYPES_H
