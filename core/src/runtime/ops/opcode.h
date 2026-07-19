#ifndef OPCODES_TYPES_H
#define OPCODES_TYPES_H

/**
 * Коды инструкций виртуальной машины.
 *
 * Opcode определяет, что должна сделать VM.
 * Конкретный оператор (OperatorID) вызывается только инструкцией OP_CALL. */
typedef enum {
    OP_NOP = 0,
    OP_HALT,
    /* Работа с регистрами */
    OP_LOAD_CONST,
    OP_MOVE,
    OP_ADD,
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
    OP_EXEC_ALGORITHM,   // выполнить алгоритм по ID из регистра
    /* Количество opcode */
    VM_OPCODE_COUNT,
    /* опкоды для работы с графом */
    OP_FETCH_NODE,  // достать факт
    OP_CHECK_EDGE,  // проверить связь
    OP_ASSERT,      // выдвинуть гипотезу — временно записывает в working_memory
    OP_BACKTRACK,    // откатить состояние, если гипотеза зашла в тупик
    OP_GET_EDGE,
    OP_SET_TMP,
    OP_COMMIT,
    OP_CHECK_CACHED_EDGE
} Opcode;

#endif
