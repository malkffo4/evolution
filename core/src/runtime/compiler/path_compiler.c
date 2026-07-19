// runtime/compiler/path_compiler.c
#include <stdlib.h>
#include "runtime/compiler/pipeline.h"
#include "runtime/vm/instruction.h"
#include "runtime/ops/opcode.h"
#include "storage/graph/graph.h"

// Простейший компилятор пути: для каждого шага проверяем связь
// и записываем промежуточный результат в scratchpad.
// Вход: массив node_id – цепочка узлов, count – их количество.
// Выход: Pipeline, который можно сразу подать в vm_execute.
Pipeline* compile_path_to_pipeline(node_id_t* path, int count) {
    if (count < 2) return NULL;

    Pipeline *p = pipeline_create();
    if (!p) return NULL;

    // Константы: идентификаторы отношений будем брать из пула констант
    // Для простоты захардкодим relation = 1 (CAUSES)
    p->constants.int_consts = malloc(sizeof(int64_t));
    p->constants.int_consts[0] = 1;   // ID отношения CAUSES
    p->constants.int_count = 1;

    for (int i = 0; i < count - 1; i++) {
        // Загрузить текущий узел в регистр R0
        Instruction load_src = {
            .operator_id = OP_LOAD_CONST,
            .arg[0] = 0,               // регистр R0
            .arg[1] = (uint32_t)path[i]  // сам ID узла (временно, нужна константа)
        };
        // Загрузить следующий узел в R1
        Instruction load_tgt = {
            .operator_id = OP_LOAD_CONST,
            .arg[0] = 1,
            .arg[1] = (uint32_t)path[i+1]
        };
        // Загрузить отношение CAUSES в R2 из константы 0
        Instruction load_rel = {
            .operator_id = OP_LOAD_CONST,
            .arg[0] = 2,
            .arg[1] = 0   // индекс константы 0
        };
        // Проверить связь: R3 = GET_EDGE(R0, R2, R1)
        Instruction get_edge = {
            .operator_id = OP_GET_EDGE,   // надо добавить в opcode.h
            .arg[0] = 3,   // результат
            .arg[1] = 0,   // src
            .arg[2] = 2,   // rel
            .arg[3] = 1    // tgt
        };
        // Сохранить результат в scratchpad с ключом i
        Instruction set_tmp = {
            .operator_id = OP_SET_TMP,
            .arg[0] = i,   // ключ – номер шага
            .arg[1] = 3    // значение из R3
        };

        pipeline_add_instruction(p, &load_src);
        pipeline_add_instruction(p, &load_tgt);
        pipeline_add_instruction(p, &load_rel);
        pipeline_add_instruction(p, &get_edge);
        pipeline_add_instruction(p, &set_tmp);
    }

    // HALT
    Instruction halt = { .operator_id = OP_HALT };
    pipeline_add_instruction(p, &halt);

    return p;
}
