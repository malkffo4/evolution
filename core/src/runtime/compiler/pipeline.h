// Pipeline Instruction ConstantPool ExecutionPlan
#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdint.h>

#include "runtime/vm/instruction.h"
#include "pipline_types.h"

#define MAX_OPERATORS_IN_PIPELINE 200

/* Строковое представление */
typedef struct {
    const char *data;
    uint32_t    len;
} StringView;

/* Таблица констант (для VM) */
typedef struct {
    int64_t *int_consts;
    double  *float_consts;
    StringView *str_consts;
    uint32_t int_count;
    uint32_t float_count;
    uint32_t str_count;
} ConstantPool;

/* Пайплайн (граф обработки) */
typedef struct Pipeline {
    PipelineID      id;
    Instruction     *code;          // байткод
    uint32_t        code_len;
    ConstantPool    constants;    // пул констант
    // const Operator *resolved[MAX_OPERATORS_IN_PIPELINE];
    /* могут быть другие поля */
} Pipeline;

Pipeline* pipeline_create(void);
void pipeline_add_instruction(Pipeline *p, const Instruction ins);
void pipeline_free(Pipeline *p);

#endif // PIPELINE_H
