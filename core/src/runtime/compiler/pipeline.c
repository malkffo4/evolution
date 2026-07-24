// runtime/compiler/pipeline.c
#include <stdlib.h>

#include "runtime/vm/vm_status.h"
#include "runtime/compiler/pipeline.h"

Pipeline *pipeline_create(void) {
    Pipeline *p = calloc(1, sizeof(Pipeline));

    if (!p)
        return NULL;

    p->code = calloc(MAX_PIPELINE_CODE, sizeof(Instruction));

    if (!p->code) {
        free(p);
        return NULL;
    }

    p->capacity = MAX_PIPELINE_CODE;
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
    if (p == NULL)
        return;
    if (p->code)
        free(p->code);
    free(p);
}
