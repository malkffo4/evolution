// knowledge/pipeline_io.c
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "pipeline_io.h"
#include "knowledge/algorithm_saver.h"
#include "runtime/operator/operator.h"
#include "runtime/logging/logging.h"
#include "math/hash.h"

static bool parse_constant_pool(cJSON *const_json, ConstantPool *c) {
    if (!cJSON_IsObject(const_json)) return false;
    cJSON *item;
    // int_consts
    item = cJSON_GetObjectItem(const_json, "int_consts");
    if (cJSON_IsArray(item)) {
        c->int_count = cJSON_GetArraySize(item);
        c->int_consts = malloc(c->int_count * sizeof(int64_t));
        for (uint32_t i = 0; i < c->int_count; i++)
            c->int_consts[i] = (int64_t)cJSON_GetNumberValue(cJSON_GetArrayItem(item, i));
    }
    // float_consts
    item = cJSON_GetObjectItem(const_json, "float_consts");
    if (cJSON_IsArray(item)) {
        c->float_count = cJSON_GetArraySize(item);
        c->float_consts = malloc(c->float_count * sizeof(double));
        for (uint32_t i = 0; i < c->float_count; i++)
            c->float_consts[i] = cJSON_GetNumberValue(cJSON_GetArrayItem(item, i));
    }
    // str_consts
    item = cJSON_GetObjectItem(const_json, "str_consts");
    if (cJSON_IsArray(item)) {
        c->str_count = cJSON_GetArraySize(item);
        c->str_consts = malloc(c->str_count * sizeof(StringView));
        for (uint32_t i = 0; i < c->str_count; i++) {
            const char *s = cJSON_GetStringValue(cJSON_GetArrayItem(item, i));
            if (s) {
                c->str_consts[i].len = (uint32_t)strlen(s);
                c->str_consts[i].data = strdup(s);
            }
        }
    }
    return true;
}

int pipeline_import_json(const char *json_str, MDB_txn *txn) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return -1;

    const char *algo_name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "algo_name"));
    uint64_t algo_id = djb2_hash(algo_name);
    cJSON *code_arr = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsArray(code_arr)) { cJSON_Delete(root); return -1; }

    uint32_t len = cJSON_GetArraySize(code_arr);
    Instruction *code = malloc(len * sizeof(Instruction));
    for (uint32_t i = 0; i < len; i++) {
        cJSON *ins = cJSON_GetArrayItem(code_arr, i);
        const char *op_name = cJSON_GetStringValue(cJSON_GetObjectItem(ins, "operator_id"));
        code[i].operator_id = operator_find_by_name(op_name);
        cJSON *arg_arr = cJSON_GetObjectItem(ins, "arg");
        for (int a = 0; a < 6; a++)
            code[i].arg[a] = (uint32_t)cJSON_GetNumberValue(cJSON_GetArrayItem(arg_arr, a));
    }

    Pipeline p = { .code = code, .code_len = len, .capacity = len };
    parse_constant_pool(cJSON_GetObjectItem(root, "constants"), &p.constants);

    int rc = algorithm_save(txn, algo_id, &p);
    free(code);
    cJSON_Delete(root);
    return rc;
}
