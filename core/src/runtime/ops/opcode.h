#ifndef OPCODES_TYPES_H
#define OPCODES_TYPES_H

/* Коды инструкций виртуальной машины.
 *
 * Opcode определяет, что должна сделать VM.
 * Конкретный оператор (OperatorID) вызывается только инструкцией OP_CALL.
 */
typedef enum {
    OP_NOP = 0,
    OP_HALT,

    /* Работа с регистрами */
    OP_LOAD_CONST,
    OP_MOVE,
    OP_STORE,
    OP_CLEAR,

    /* Вызовы */
    OP_CALL,
    OP_CALL_INDIRECT,
    OP_RETURN,

    /* Управление потоком */
    OP_BRANCH,
    OP_BRANCH_IF_TRUE,
    OP_BRANCH_IF_FALSE,
    OP_BRANCH_IF_EMPTY,

    /* Графовые операции */
    OP_COMPUTE_SIMILARITY,
    OP_CREATE_HYPOTHESIS,
    OP_EXTRACT_TARGETS,
    OP_GET_NODE,
    OP_GET_IN_EDGES,
    OP_GET_INCOMING,
    OP_GET_OUT_EDGES,
    OP_GET_OUTGOING,
    OP_FILTER,
    OP_FILTER_RELATION,
    OP_INTERSECTION,
    OP_UNION,
    OP_DIFFERENCE,
    OP_MATCH_GREEDY,
    OP_MATCH_HUNGARIAN,
    OP_SCORE,
    OP_SORT,
    OP_PUSH_RESULT,

    /* Количество opcode */
    VM_OPCODE_COUNT
} Opcode;

#endif
