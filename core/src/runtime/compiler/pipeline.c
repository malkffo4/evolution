// runtime/compiler/pipeline.c
#include <stdlib.h>

#include "runtime/vm/vm_status.h"
#include "runtime/compiler/pipeline.h"

Pipeline *pipeline_create(void) {
    return pipeline_create_with_capacity(MAX_PIPELINE_CODE);
}

Pipeline *pipeline_create_with_capacity(int capacity) {
    if (capacity <= 0 || capacity > MAX_PIPELINE_CODE)
        return NULL;

    Pipeline *p = calloc(1, sizeof(Pipeline));
    if (!p)
        return NULL;

    p->code = calloc(capacity, sizeof(Instruction));
    if (!p->code) {
        free(p);
        return NULL;
    }

    p->capacity = capacity;
    p->code_len = 0;

    return p;
}

VMStatus pipeline_add_instruction(Pipeline *p, const Instruction *ins) {
    if (p->code_len >= p->capacity)
        return VM_OUT_OF_MEMORY;

    p->code[p->code_len++] = *ins;

    return VM_OK;
}

void pipeline_free(Pipeline *p) {
    if (!p) return;

    // Освобождение байткода
    if (p->code) {
        free(p->code);
        p->code = NULL;
    }

    // Глубокая очистка пула констант
    if (p->constants.int_consts) {
        free(p->constants.int_consts);
        p->constants.int_consts = NULL;
    }
    if (p->constants.float_consts) {
        free(p->constants.float_consts);
        p->constants.float_consts = NULL;
    }
    if (p->constants.str_consts) {
        // Освобождаем каждую строку (выделенную через strdup)
        for (uint32_t i = 0; i < p->constants.str_count; i++) {
            if (p->constants.str_consts[i].data) {
                free(p->constants.str_consts[i].data);
            }
        }
        // Освобождаем сам массив указателей
        free(p->constants.str_consts);
        p->constants.str_consts = NULL;
    }
    p->constants.int_count = 0;
    p->constants.float_count = 0;
    p->constants.str_count = 0;

    free(p);
}
