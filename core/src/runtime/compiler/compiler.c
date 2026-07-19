#include <stdlib.h>
#include <string.h>

#include "runtime/compiler/pipeline.h"
#include "runtime/operator/operator.h"

// Временные определения для компиляции
typedef struct GraphNode {
    OperatorID operator_id;
    uint32_t arg[6];
} GraphNode;

typedef struct ExecutionGraph {
    GraphNode *nodes;
    uint32_t node_count;
} ExecutionGraph;

// Простейший компилятор, который создаёт Pipeline с заготовленным кодом.
// В реальности он должен обходить граф операций и генерировать байткод.
Pipeline *compiler_compile(const ExecutionGraph *graph) {
    Pipeline *p = pipeline_create();

    if (!p)
        return NULL;

    for (uint32_t i = 0; i < graph->node_count; ++i) {
        const GraphNode *node = &graph->nodes[i];

        const Operator *op = operator_find(node->operator_id);

        if (!op)
            continue;

        Instruction ins = {
            .operator_id = op->id,
            .capability_mask = 0,
            .flags = 0,
        };

        memcpy(ins.arg, node->arg, sizeof(ins.arg));

        pipeline_add_instruction(p, &ins);
    }

    return p;
}

// Не Pipeline а ExecutionPlan
// Pipeline
// ↓
// Compiler
// ↓
// ExecutionPlan
// ↓
// VM

// Потому что потом Compiler сможет
// constant folding
// dead code elimination
// register allocation
// jump optimization
// operator fusion
// cache

// VM вообще не должна этим заниматься.

// Я бы уже сейчас строил систему как пятиуровневую:

// Knowledge Graph
//         │
//         ▼
// Reasoning Graph (Pipeline)
//         │
//         ▼
// Compiler
//         │
//         ▼
// ExecutionPlan
//         │
//         ▼
// VM

// Именно такое разделение позволит позже добавить оптимизации, JIT,
// автоматическое переписывание пайплайнов и самообучение без изменения самой VM.
// На мой взгляд, это наиболее масштабируемая архитектура для той системы

// TODO Задачи:
// Напишите функцию, которая обходит выгруженный из БД подграф и мапит типы узлов на опкоды.
// Узел типа "Условие" становится OP_BRANCH. Узел "Действие" становится OP_CALL.
