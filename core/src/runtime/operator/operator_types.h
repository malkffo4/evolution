// runtime/operator/operator_types.h
#pragma once

#include <stdint.h>

typedef uint32_t OperatorID;

typedef enum {
    OPERATOR_NATIVE,
    OPERATOR_PIPELINE,
    OPERATOR_COMPILED,
    OPERATOR_GPU
} OperatorKind;
