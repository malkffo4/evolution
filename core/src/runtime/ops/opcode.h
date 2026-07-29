// runtime/ops/opcode.h
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
    OP_GET_EDGE,
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
    OP_EXEC_ALGORITHM_BY_GOAL,
    OP_GET_NEIGHBORS,
    OP_FIND_SIMILAR,
    OP_CONCAT_PATHS,
    OP_PROP_SET,
    OP_PROP_GET,
    /* опкоды для работы с графом */
    OP_FETCH_NODE,  // достать факт
    OP_CHECK_EDGE,  // проверить связь
    OP_CHECK_CACHED_EDGE,
    OP_BACKTRACK,    // откатить состояние, если гипотеза зашла в тупик
    OP_SET_TMP,
    OP_COMMIT,

    // --- ПРИЧИННО-СЛЕДСТВЕННЫЙ ДВИЖОК (HYPER-OPS) ---

    OP_QUERY, // (process_id, arg_filter, context_id) -> Массив ID в scratchpad

    OP_ASSERT, // Создает новый факт. ID генерируется, cause_id берется из текущего эпизода

    OP_DERIVE, // Как ASSERT, но cause_id = ID аргументов (указывает логический вывод)

    OP_TRACE, // Выгружает цепочку cause_id (Откуда мы это знаем?)

    // --- УПРАВЛЕНИЕ РЕАЛЬНОСТЬЮ (КОНТЕКСТАМИ) ---
    OP_SPAWN_CTX, // Создает ветку реальности (гипотезу)
    OP_MERGE_CTX, // Переносит успешную гипотезу в родительский контекст
    OP_CURRENT_CTX, // Загружает ID текущего контекста в регистр

    // Базовая арифметика и переходы
    OP_READ_SP,
    OP_WRITE_SP,
    OP_ADD,
    OP_ADD_CONST,
    OP_JGE,
    OP_JMP,

    OP_SPREAD_ACTIVATION,
    OP_EVALUATE_GOALS,

    OP_LOAD_CONTEXT,
    OP_READ_FAILURES,   // arg[0]=sp_offset, arg[1]=count_reg -> scratchpad[algo_id, fail_count]...

    OP_VECTOR_SIM,
    OP_CRITIC_APPLY,

    OP_MATCH_PATTERN,
    OP_CREDIT_ASSIGN,          // NEW: RFC-0001 credit assignment по idx_causal
    /* Количество opcode */
    VM_OPCODE_COUNT
} Opcode;

#endif // OPCODES_TYPES_H
