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
#include "storage/hyper_atom/hyper_atom.h"

static int find_var(cJSON *vars_arr, const char *name) {
    int idx = 0;
    cJSON *v;
    cJSON_ArrayForEach(v, vars_arr) {
        if (cJSON_IsString(v) && strcmp(v->valuestring, name) == 0) return idx;
        idx++;
    }
    return -1;
}

static bool parse_constant_pool(cJSON *const_json, ConstantPool *c) {
    memset(c, 0, sizeof(ConstantPool));
    if (!cJSON_IsObject(const_json)) return false;

    cJSON *item = cJSON_GetObjectItem(const_json, "int_consts");
    if (cJSON_IsArray(item)) {
        c->int_count = (uint32_t)cJSON_GetArraySize(item);
        if (c->int_count > 0) {
            c->int_consts = calloc(c->int_count, sizeof(int64_t));
            if (!c->int_consts) goto cleanup;
            for (uint32_t i = 0; i < c->int_count; i++)
                c->int_consts[i] = (int64_t)cJSON_GetNumberValue(cJSON_GetArrayItem(item, (int)i));
        }
    }

    item = cJSON_GetObjectItem(const_json, "float_consts");
    if (cJSON_IsArray(item)) {
        c->float_count = (uint32_t)cJSON_GetArraySize(item);
        if (c->float_count > 0) {
            c->float_consts = calloc(c->float_count, sizeof(double));
            if (!c->float_consts) goto cleanup;
            for (uint32_t i = 0; i < c->float_count; i++)
                c->float_consts[i] = cJSON_GetNumberValue(cJSON_GetArrayItem(item, (int)i));
        }
    }

    item = cJSON_GetObjectItem(const_json, "str_consts");
    if (cJSON_IsArray(item)) {
        c->str_count = (uint32_t)cJSON_GetArraySize(item);
        if (c->str_count > 0) {
            c->str_consts = calloc(c->str_count, sizeof(StringView));
            if (!c->str_consts) goto cleanup;
            for (uint32_t i = 0; i < c->str_count; i++) {
                const char *s = cJSON_GetStringValue(cJSON_GetArrayItem(item, (int)i));
                if (s) {
                    c->str_consts[i].len = (uint32_t)strlen(s);
                    c->str_consts[i].data = strdup(s);
                    if (!c->str_consts[i].data) goto cleanup;
                }
            }
        }
    }
    return true;
cleanup:
    return false;
}

Pipeline* pipeline_from_json(cJSON *root, uint64_t *out_algo_id) {
    if (!root) return NULL;

    const char *algo_name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "algo_name"));
    if (algo_name && out_algo_id) {
        *out_algo_id = djb2_hash(algo_name);
    }

    cJSON *code_arr = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsArray(code_arr)) return NULL;

    uint32_t len = (uint32_t)cJSON_GetArraySize(code_arr);
    Pipeline *p = pipeline_create_with_capacity(len);
    if (!p) return NULL;

    p->code_len = len;

    if (len > 0) {
        for (uint32_t i = 0; i < len; i++) {
            cJSON *ins = cJSON_GetArrayItem(code_arr, (int)i);
            const char *op_name = cJSON_GetStringValue(cJSON_GetObjectItem(ins, "operator_id"));
            p->code[i].operator_id = operator_find_by_name(op_name);

            // Безопасный парсинг аргументов, не падающий при коротких массивах или числах
            cJSON *arg_item = cJSON_GetObjectItem(ins, "arg");
            if (arg_item) {
                if (cJSON_IsArray(arg_item)) {
                    int arg_sz = cJSON_GetArraySize(arg_item);
                    for (int a = 0; a < 6; a++) {
                        if (a < arg_sz) {
                            cJSON *item = cJSON_GetArrayItem(arg_item, a);
                            p->code[i].arg[a] = cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 0;
                        } else {
                            p->code[i].arg[a] = 0;
                        }
                    }
                } else if (cJSON_IsNumber(arg_item)) {
                    p->code[i].arg[0] = (uint32_t)arg_item->valuedouble;
                    for (int a = 1; a < 6; a++) p->code[i].arg[a] = 0;
                } else {
                    for (int a = 0; a < 6; a++) p->code[i].arg[a] = 0;
                }
            } else {
                for (int a = 0; a < 6; a++) p->code[i].arg[a] = 0;
            }
        }
    }

    bool rc = parse_constant_pool(cJSON_GetObjectItem(root, "constants"), &p->constants);
    if (!rc) goto cleanup;

    return p;

cleanup:
    pipeline_free(p);
    return NULL;
}

// Args schema: {"var": "name"} | {"const": "STRING_TO_HASH"} | {"any": true}
int hyper_pattern_from_json(cJSON *root, HyperPattern *out) {
    if (!root || !out) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *id_json = cJSON_GetObjectItem(root, "pattern_id");
    if (!cJSON_IsNumber(id_json)) return -1;
    out->id = (ko_id_t)id_json->valuedouble;

    cJSON *vars_arr = cJSON_GetObjectItem(root, "vars");
    out->var_count = cJSON_IsArray(vars_arr) ? (uint32_t)cJSON_GetArraySize(vars_arr) : 0;
    if (out->var_count > MAX_PATTERN_VARS) return -1;

    cJSON *cond_arr = cJSON_GetObjectItem(root, "conditions");
    if (!cJSON_IsArray(cond_arr)) return -1;
    int cc = cJSON_GetArraySize(cond_arr);
    if (cc <= 0 || cc > MAX_PATTERN_CONDITIONS) return -1;
    out->condition_count = (uint32_t)cc;

    for (int i = 0; i < cc; i++) {
        cJSON *c = cJSON_GetArrayItem(cond_arr, i);
        PatternCondition *pc = &out->conditions[i];

        cJSON *proc = cJSON_GetObjectItem(c, "process");
        if (!cJSON_IsString(proc)) return -1;
        pc->process_id = djb2_hash(proc->valuestring);

        cJSON *args = cJSON_GetObjectItem(c, "args");
        if (!cJSON_IsArray(args) || cJSON_GetArraySize(args) != PATTERN_ARG_SLOTS) return -1;

        for (int s = 0; s < PATTERN_ARG_SLOTS; s++) {
            cJSON *a = cJSON_GetArrayItem(args, s);
            cJSON *var = cJSON_GetObjectItem(a, "var");
            cJSON *cst = cJSON_GetObjectItem(a, "const");
            cJSON *any = cJSON_GetObjectItem(a, "any");

            if (cJSON_IsString(var)) {
                int vi = find_var(vars_arr, var->valuestring);
                if (vi < 0) return -1;
                pc->kind[s] = PARG_VAR;
                pc->var_index[s] = (uint8_t)vi;
            } else if (cJSON_IsString(cst)) {
                pc->kind[s] = PARG_CONST;
                pc->value[s] = HYPER_MAKE_REF(djb2_hash(cst->valuestring));
            } else if (any && cJSON_IsTrue(any)) {
                pc->kind[s] = PARG_ANY;
            } else {
                return -1;
            }
        }
    }
    return 0;
}
