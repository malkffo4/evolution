// runtime/compiler/pipeline.h
#pragma once

#include <stdint.h>

#include "runtime/vm/vm_status.h"
#include "runtime/vm/instruction.h"
#include "pipline_types.h"

#define MAX_OPERATORS_IN_PIPELINE   200
#define MAX_PIPELINE_CODE           1024

/* Строковое представление */
typedef struct {
    char *data;
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
    uint32_t        capacity; // WHY?
    uint32_t        code_len;
    ConstantPool    constants;    // пул констант
    // const Operator *resolved[MAX_OPERATORS_IN_PIPELINE];
    /* могут быть другие поля */
} Pipeline;

Pipeline* pipeline_create(void);
Pipeline *pipeline_create_with_capacity(int capacity);
VMStatus pipeline_add_instruction(Pipeline *p, const Instruction *ins);
void pipeline_free(Pipeline *p);
