// runtime/operator/operator_types.h
#ifndef OPERATOR_TYPES_H
#define OPERATOR_TYPES_H

#include <stdint.h>

typedef uint32_t OperatorID;

typedef enum {
    OPERATOR_NATIVE,
    OPERATOR_PIPELINE,
    OPERATOR_COMPILED,
    OPERATOR_GPU
} OperatorKind;

#endif
