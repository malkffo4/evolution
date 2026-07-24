// knowledge/pipeline_io.c
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "pipeline_io.h"
#include "runtime/operator/operator.h"
#include "runtime/logging/logging.h"
#include "math/hash.h"

static bool parse_constant_pool(cJSON *const_json, ConstantPool *c) {
    memset(c, 0, sizeof(ConstantPool));
    if (!cJSON_IsObject(const_json)) return false;

    cJSON *item = cJSON_GetObjectItem(const_json, "int_consts");
    if (cJSON_IsArray(item)) {
        c->int_count = cJSON_GetArraySize(item);
        if (c->int_count > 0) {
            c->int_consts = calloc(c->int_count, sizeof(int64_t));
            for (uint32_t i = 0; i < c->int_count; i++)
                c->int_consts[i] = (int64_t)cJSON_GetNumberValue(cJSON_GetArrayItem(item, i));
        }
    }

    item = cJSON_GetObjectItem(const_json, "float_consts");
    if (cJSON_IsArray(item)) {
        c->float_count = cJSON_GetArraySize(item);
        if (c->float_count > 0) {
            c->float_consts = calloc(c->float_count, sizeof(double));
            for (uint32_t i = 0; i < c->float_count; i++)
                c->float_consts[i] = cJSON_GetNumberValue(cJSON_GetArrayItem(item, i));
        }
    }

    item = cJSON_GetObjectItem(const_json, "str_consts");
    if (cJSON_IsArray(item)) {
        c->str_count = cJSON_GetArraySize(item);
        if (c->str_count > 0) {
            c->str_consts = calloc(c->str_count, sizeof(StringView));
            for (uint32_t i = 0; i < c->str_count; i++) {
                const char *s = cJSON_GetStringValue(cJSON_GetArrayItem(item, i));
                if (s) {
                    c->str_consts[i].len = (uint32_t)strlen(s);
                    c->str_consts[i].data = strdup(s);
                }
            }
        }
    }
    return true;
}

Pipeline* pipeline_from_json(cJSON *root, uint64_t *out_algo_id) {
    if (!root) return NULL;

    const char *algo_name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "algo_name"));
    if (algo_name && out_algo_id) {
        *out_algo_id = djb2_hash(algo_name);
    }

    cJSON *code_arr = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsArray(code_arr)) return NULL;

    uint32_t len = cJSON_GetArraySize(code_arr);
    Pipeline *p = calloc(1, sizeof(Pipeline));
    if (!p) return NULL;

    p->code_len = len;
    p->capacity = len;
    if (len > 0) {
        p->code = calloc(len, sizeof(Instruction));
        for (uint32_t i = 0; i < len; i++) {
            cJSON *ins = cJSON_GetArrayItem(code_arr, i);
            const char *op_name = cJSON_GetStringValue(cJSON_GetObjectItem(ins, "operator_id"));
            p->code[i].operator_id = operator_find_by_name(op_name);
            cJSON *arg_arr = cJSON_GetObjectItem(ins, "arg");
            for (int a = 0; a < 6; a++) {
                p->code[i].arg[a] = (uint32_t)cJSON_GetNumberValue(cJSON_GetArrayItem(arg_arr, a));
            }
        }
    }

    parse_constant_pool(cJSON_GetObjectItem(root, "constants"), &p->constants);
    return p;
}
