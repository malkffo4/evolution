// runtime/expression/expression_node.h
#ifndef EXPRESSION_H
#define EXPRESSION_H

#include <stdint.h>

typedef uint32_t ExprNodeID;

typedef enum {
    EXPR_CONSTANT,
    EXPR_INPUT,
    EXPR_ADD,
    EXPR_SUB,
    EXPR_MUL,
    EXPR_DIV,
    EXPR_MIN,
    EXPR_MAX,
    EXPR_AVG,
    EXPR_CLAMP,
    EXPR_LERP,
    EXPR_IF,
    EXPR_CALL
} ExprOp;

typedef struct {
    ExprOp op;
    ExprNodeID input[4];
    uint8_t input_count;
    float value;
    uint32_t userdata;
} ExprNode;

typedef struct {
    ExprNode *nodes;
    uint32_t count;
    uint32_t capacity;
    ExprNodeID output;
} ExpressionGraph;

#endif
