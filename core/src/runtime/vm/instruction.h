// runtime/vm/instruction.h
#pragma once

#include <stdint.h>
#include "runtime/capability/capability_types.h"
#include "runtime/operator/operator_types.h"

/* Instruction.flags — биты модификаторов выполнения.
 *
 * INS_FLAG_SOFT_FAIL:
 *   Если инструкция вернула VM_NOT_FOUND, vm_execute() НЕ обрывает
 *   Pipeline (в отличие от любого другого ненулевого статуса). Нужно
 *   для шагов вида "могло не найтись — и это ожидаемо": OP_EVALUATE_GOALS
 *   в MainLoop не обязан находить цель на каждом тике, и это не должно
 *   отменять идущие следом в том же Pipeline OP_SPREAD_ACTIVATION и
 *   запуск CriticMain.
 * Формат сериализации Instruction не меняется — поле flags уже
 * существовало и было нулевым у всех ранее сохранённых алгоритмов.
 */
#define INS_FLAG_SOFT_FAIL  0x0001u

/*
 * Универсальная инструкция VM.
 *
 * Выполнение:
 *   operator != 0
 *       Прямой вызов зарегистрированного оператора.
 *
 *   operator == 0
 *       Планировщик выбирает подходящий оператор по capability_mask.
 *
 * arg[] — универсальные операнды инструкции.
 *
 * VM не интерпретирует их содержимое.
 * Их семантика полностью определяется конкретным оператором.
 *
 * Рекомендуемое соглашение:
 *
 *   arg[0] — основной операнд
 *            (регистр, дескриптор, индекс объекта и т.п.)
 *
 *   arg[1] — второй операнд
 *            (регистр, адрес перехода, размер и т.п.)
 *
 *   arg[2] — третий операнд
 *
 *   arg[3] — четвертый операнд
 *
 *   arg[4] — пятый операнд
 *
 *   arg[5] — шестой операнд / служебные данные
 *
 * Примеры:
 *
 * BRANCH
 *   arg[0] = target_ip
 *
 * BRANCH_IF_EMPTY
 *   arg[0] = register
 *   arg[1] = target_ip
 *
 * CALL
 *   arg[0] = function_id
 *   arg[1] = first_argument
 *   arg[2] = argument_count
 *
 * GRAPH_MATCH
 *   arg[0] = query_node
 *   arg[1] = result_register
 *
 * Таким образом формат Instruction остается неизменным для любых
 * операторов, а интерпретация arg[] находится исключительно в
 * реализации соответствующего оператора.
 */
typedef struct Instruction {
    OperatorID      operator_id;         // 0 -> выбрать оператор по capability_mask
    CapabilityMask  capability_mask;  // используется только если operator == 0
    uint16_t        flags;            // модификаторы выполнения
    uint32_t        arg[6];           // параметры оператора
} Instruction;

// Структура инструкции (фиксированный размер для простоты парсинга)
// typedef struct {
//     uint8_t opcode;
//     uint8_t arg0;
//     uint8_t arg1;
//     int32_t imm;     // Иммедиат (для констант или адресов переходов)
// } Instruction;
