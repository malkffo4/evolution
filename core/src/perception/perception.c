// perception/perception.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/globals.h"
#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/graph/graph.h"
#include "storage/string_pool/string_pool.h"
#include "storage/hyper_atom/hyper_atom.h"
#include "storage/property/property.h"
#include <cjson/cJSON.h>
#include "math/hash.h"
#include "runtime/logging/logging.h"
#include "runtime/operator/operator.h"

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Загрузка знаний из JSON напрямую в Рабочую Память (Working Memory)
int perceive_and_activate(const char *json_str, WorkingMemory *wm, MDB_txn *txn, HyperMemory *hmem) {
    cJSON *json = cJSON_Parse(json_str);
    if (json == NULL) return -1;

    // 1. Считываем узлы и их когнитивные эмоции
    cJSON *nodes = cJSON_GetObjectItemCaseSensitive(json, "nodes");
    cJSON *node = NULL;

    if (cJSON_IsArray(nodes)) {
        cJSON_ArrayForEach(node, nodes) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(node, "id");
            cJSON *label = cJSON_GetObjectItemCaseSensitive(node, "label");

            // Читаем эмоции, если нейронка смогла их извлечь
            cJSON *danger_json = cJSON_GetObjectItemCaseSensitive(node, "danger");
            cJSON *utility_json = cJSON_GetObjectItemCaseSensitive(node, "utility");

            // ИСПРАВЛЕНИЕ: Правильный каст типов в тернарном операторе
            float danger = cJSON_IsNumber(danger_json) ? (float)danger_json->valuedouble : 0.1f;
            float utility = cJSON_IsNumber(utility_json) ? (float)utility_json->valuedouble : 0.1f;

            if (cJSON_IsString(id) && cJSON_IsString(label)) {
                uint64_t node_id = djb2_hash(id->valuestring);
                Node c_node = {
                    .id = node_id,
                    .name_hash = add_string_to_pool(txn, label->valuestring)
                };
                create_node(txn, &c_node);

                // Загружаем узел в оперативную память и передаем ему ЭМОЦИИ
                wm_activate(wm, node_id, 1.0f, danger); // Используем danger как когнитивный вес

                // Находим этот узел в WM и прописываем тонкие эмоции
                for (uint32_t i = 0; i < wm->count; i++) {
                    if (wm->nodes[i].node_id == node_id) {
                        wm->nodes[i].state.danger = danger;
                        wm->nodes[i].state.usefulness = utility;
                        break;
                    }
                }
                LOG_PERCEPTION("[ВОСПРИЯТИЕ] Узел '%s' (Опасность: %.2f, Польза: %.2f)", label->valuestring, danger, utility);
            }
        }
    }

    // 2. Считываем связи и записываем их в базу (с Байесовским обновлением)
    cJSON *edges = cJSON_GetObjectItemCaseSensitive(json, "edges");
    cJSON *edge = NULL;

    if (cJSON_IsArray(edges)) {
        cJSON_ArrayForEach(edge, edges) {
            cJSON *source = cJSON_GetObjectItemCaseSensitive(edge, "source");
            cJSON *target = cJSON_GetObjectItemCaseSensitive(edge, "target");
            cJSON *relation = cJSON_GetObjectItemCaseSensitive(edge, "relation");

            if (cJSON_IsString(source) && cJSON_IsString(target) && cJSON_IsString(relation)) {
                Edge logic_edge;
                logic_edge.key.source = djb2_hash(source->valuestring);
                logic_edge.key.target = djb2_hash(target->valuestring);
                logic_edge.key.relation = add_string_to_pool(txn, relation->valuestring);
                logic_edge.confidence = 0.5f;
                logic_edge.context = 0;
                upsert_edge(txn, &logic_edge);

                add_string_to_pool(txn, "EDGE"); // чтобы process-label резолвился при retrieve

                // NeuroAtom edge_atom = {
                //     .id = 0, // будет присвоен в hyper_assert_with_cause
                //     .process_id =
                //         proc_make(logic_edge.key.relation, PROC_KIND_RELATION),
                //     .args = {{.raw = HYPER_MAKE_REF(logic_edge.key.source)},
                //              {.raw = HYPER_MAKE_REF(logic_edge.key.target)}},
                //     .context_or_time_link = 0,

                //     // Дефолтные значения векторов для новых связей из Perception
                //     .truth_mean = 1.0f,
                //     .truth_confidence = 0.5f,
                //     .sti = 0.8f, // Горячий факт
                //     .lti = 0.1f,
                //     .utility = 0.0f,
                //     .valence = 0.0f};

                static uint64_t next_edge_id = 10000;
                // edge_atom.id = ++next_edge_id;

                // // Используем 0 как cause_id (внешнее восприятие)
                // hyper_assert_with_cause(txn, hmem, &edge_atom, 0);
                //
                // Вместо одного атома с process_id = relation, создаём два
                NeuroAtom fwd = {0};
                fwd.id = next_edge_id++;
                fwd.process_id = djb2_hash("EDGE_FWD");
                fwd.args[0].raw = HYPER_MAKE_REF(djb2_hash(source->valuestring));
                fwd.args[1].raw = HYPER_MAKE_REF(djb2_hash(relation->valuestring));
                fwd.truth_mean = 1.0f;
                fwd.truth_confidence = 0.5f;
                hyper_assert_unique(txn, hmem, &fwd);

                NeuroAtom rev = {0};
                rev.id = next_edge_id++;
                rev.process_id = djb2_hash("EDGE_REV");
                rev.args[0].raw = HYPER_MAKE_REF(djb2_hash(relation->valuestring));
                rev.args[1].raw = HYPER_MAKE_REF(djb2_hash(target->valuestring));
                rev.truth_mean = 1.0f;
                rev.truth_confidence = 0.5f;
                hyper_assert_unique(txn, hmem, &rev);
            }
        }
    }

    cJSON_Delete(json);
    return 0;
}

static ko_id_t resolve_arg(cJSON *arg_item) {
    if (cJSON_IsString(arg_item)) {
        const char *str = arg_item->valuestring;
        // Проверяем, не число ли это в строке
        char *end;
        long long ival = strtoll(str, &end, 10);
        if (*end == '\0') return (ko_id_t)(ival) | HYPER_TYPE_INT;
        double dval = strtod(str, &end);
        if (*end == '\0') {
            // float упаковываем: используем union
            union { double d; ko_id_t i; } u;
            u.d = dval;
            return u.i | HYPER_TYPE_FLOAT;
        }
        // Иначе хэшируем как ссылку
        return HYPER_MAKE_REF(djb2_hash(str));
    } else if (cJSON_IsNumber(arg_item)) {
        double num = arg_item->valuedouble;
        if (num == (long long)num) {
            return (ko_id_t)((long long)num) | HYPER_TYPE_INT;
        } else {
            union { double d; ko_id_t i; } u;
            u.d = num;
            return u.i | HYPER_TYPE_FLOAT;
        }
    } else if (cJSON_IsBool(arg_item)) {
        return (ko_id_t)(uint64_t)(arg_item->valueint ? 1 : 0) | HYPER_TYPE_INT;
    }
    return 0; // неизвестный тип
}

int perceive_hyper_json(const char *json_str, MDB_txn *txn, HyperMemory *hmem) {
    if (!json_str || !hmem) return -1;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) { LOG_ERROR("perceive_hyper_json: bad JSON"); return -1; }

    cJSON *atoms = cJSON_GetObjectItem(root, "atoms");
    if (!cJSON_IsArray(atoms)) { cJSON_Delete(root); return -1; }

    static uint64_t next_id = 1;
    cJSON *atom_item;

    cJSON_ArrayForEach(atom_item, atoms) {
        cJSON *process_json = cJSON_GetObjectItem(atom_item, "process");
        if (!cJSON_IsString(process_json)) continue;

        // Определяем ProcKind по полю "kind"
        ProcKind kind = PROC_KIND_ENTITY;   // по умолчанию — сущность
        cJSON *kind_json = cJSON_GetObjectItem(atom_item, "kind");
        if (cJSON_IsString(kind_json)) {
            const char *k = kind_json->valuestring;
            if      (!strcmp(k, "relation")) kind = PROC_KIND_RELATION;
            else if (!strcmp(k, "entity"))   kind = PROC_KIND_ENTITY;
            else if (!strcmp(k, "concept"))  kind = PROC_KIND_ENTITY;  // синоним
            else if (!strcmp(k, "rule"))     kind = PROC_KIND_RULE;
            else if (!strcmp(k, "goal"))     kind = PROC_KIND_GOAL;
            else if (!strcmp(k, "event"))    kind = PROC_KIND_EVENT;
            // всё остальное (skill, prediction, hypothesis, ...) — ENTITY
        }

        NeuroAtom atom = {0};
        atom.process_id = proc_make(djb2_hash(process_json->valuestring), kind);

        cJSON *args_json = cJSON_GetObjectItem(atom_item, "args");
        if (cJSON_IsArray(args_json)) {
            int n = cJSON_GetArraySize(args_json);
            for (int i = 0; i < n && i < HYPER_VAL_SLOTS; i++) {   // строго 2 слота
                atom.args[i].raw = resolve_arg(cJSON_GetArrayItem(args_json, i));
            }
        }

        // --- Epistemic Vector ---
        cJSON *truth_json = cJSON_GetObjectItem(atom_item, "truth");
        if (cJSON_IsObject(truth_json)) {
            cJSON *m = cJSON_GetObjectItem(truth_json, "mean");
            cJSON *c = cJSON_GetObjectItem(truth_json, "confidence");
            atom.truth_mean       = cJSON_IsNumber(m) ? clampf((float)m->valuedouble, 0.f, 1.f) : 1.0f;
            atom.truth_confidence = cJSON_IsNumber(c) ? clampf((float)c->valuedouble, 0.f, 1.f) : 0.5f;
        } else {
            // fuzzy-дефолт: считаем истинным, но не сильно уверенным
            atom.truth_mean = 1.0f;
            atom.truth_confidence = 0.5f;
        }

        // --- Attentional Vector ---
        cJSON *attn_json = cJSON_GetObjectItem(atom_item, "attention");
        if (cJSON_IsObject(attn_json)) {
            cJSON *sti = cJSON_GetObjectItem(attn_json, "sti");
            cJSON *lti = cJSON_GetObjectItem(attn_json, "lti");
            atom.sti = cJSON_IsNumber(sti) ? (float)sti->valuedouble : 0.5f;
            atom.lti = cJSON_IsNumber(lti) ? clampf((float)lti->valuedouble, 0.f, 1.f) : 0.1f;
        } else {
            atom.sti = 0.5f;  // новый факт по умолчанию слегка "в фокусе"
            atom.lti = 0.1f;
        }

        // --- Teleological / Affective Vector ---
        cJSON *util_json = cJSON_GetObjectItem(atom_item, "utility");
        cJSON *val_json  = cJSON_GetObjectItem(atom_item, "valence");
        atom.utility = cJSON_IsNumber(util_json) ? clampf((float)util_json->valuedouble, 0.f, 1.f) : 0.0f;
        atom.valence = cJSON_IsNumber(val_json)  ? clampf((float)val_json->valuedouble, -1.f, 1.f) : 0.0f;

        // --- context_or_time_link ---
        cJSON *ctx_json = cJSON_GetObjectItem(atom_item, "context");
        atom.context_or_time_link = ctx_json ? (ko_id_t)cJSON_GetNumberValue(ctx_json) : 0;

        // ID
        cJSON *id_json = cJSON_GetObjectItem(atom_item, "id");
        if (cJSON_IsString(id_json)) {
            atom.id = djb2_hash(id_json->valuestring);
        } else {
            atom.id = (0x2000000000000000ULL | (next_id++)) & HYPER_VALUE_MASK;
        }

        int result = hyper_assert_unique(txn, hmem, &atom);
        if (result != 0 && result != 1) {
            LOG_ERROR("Failed to assert NeuroAtom");
            continue;
        }
        // --- Открытая сумка свойств (Шаг 1: Deep Knowledge Ingestion) ---
        // Произвольные метаданные не помещаются в жёсткие 64 байта
        // NeuroAtom. Каждое поле properties{} -> отдельная запись в
        // db.graph.properties, ключ = (atom.id, djb2_hash(имя_поля)).
        cJSON *props_json = cJSON_GetObjectItem(atom_item, "properties");
        if (cJSON_IsObject(props_json)) {
            cJSON *prop;
            cJSON_ArrayForEach(prop, props_json) {
                const char *pkey = prop->string;
                if (!pkey || !pkey[0]) continue;

                if (cJSON_IsString(prop) && prop->valuestring) {
                    property_set(txn, atom.id, pkey, PROP_STRING,
                        prop->valuestring, (uint32_t)strlen(prop->valuestring) + 1);
                } else if (cJSON_IsBool(prop)) {
                    bool v = cJSON_IsTrue(prop);
                    property_set(txn, atom.id, pkey, PROP_BOOL, &v, sizeof(v));
                } else if (cJSON_IsNumber(prop)) {
                    double d = prop->valuedouble;
                    if (d == (double)(int64_t)d) {
                        int64_t v = (int64_t)d;
                        property_set(txn, atom.id, pkey, PROP_INT, &v, sizeof(v));
                    } else {
                        float v = (float)d;
                        property_set(txn, atom.id, pkey, PROP_FLOAT, &v, sizeof(v));
                    }
                } else if (cJSON_IsArray(prop) || cJSON_IsObject(prop)) {
                    // Открытая онтология: вложенные структуры не пытаемся
                    // автоматически разворачивать в отдельные NeuroAtom —
                    // сохраняем как сырой JSON-текст, ничего не теряя.
                    char *sub = cJSON_PrintUnformatted(prop);
                    if (sub) {
                        property_set(txn, atom.id, pkey, PROP_STRING, sub, (uint32_t)strlen(sub) + 1);
                        free(sub);
                    }
                }
            }
        }
        // Если kind не является базовым (т.е. это метатип вроде "skill"),
        // добавляем атом IS_A, связывающий этот объект с соответствующим концептом
        if (cJSON_IsString(kind_json)) {
            const char *k = kind_json->valuestring;
            if (strcmp(k, "relation") && strcmp(k, "entity") && strcmp(k, "concept") &&
                strcmp(k, "rule") && strcmp(k, "goal") && strcmp(k, "event")) {
                // Это расширенный метатип – создаём атом IS_A(object, Concept(k))
                NeuroAtom isa_atom = {0};
                isa_atom.id = (0x3000000000000000ULL | (next_id++)) & HYPER_VALUE_MASK;
                isa_atom.process_id = proc_make(djb2_hash("IS_A"), PROC_KIND_RELATION);
                isa_atom.args[0].raw = HYPER_MAKE_REF(atom.id);
                isa_atom.args[1].raw = HYPER_MAKE_REF(djb2_hash(k));
                isa_atom.truth_mean = 1.0f;
                isa_atom.truth_confidence = 1.0f;
                hyper_assert_unique(txn, hmem, &isa_atom);
            }
        }
        // Причинность — отдельный индекс, не в горячей структуре
        cJSON *cause_json = cJSON_GetObjectItem(atom_item, "cause");
        if (cause_json) {
            ko_id_t cause_id = cJSON_IsString(cause_json)
                ? djb2_hash(cause_json->valuestring)
                : (ko_id_t)cJSON_GetNumberValue(cause_json);
            if (cause_id) {
                MDB_val k = { sizeof(ko_id_t), &atom.id };
                MDB_val v = { sizeof(ko_id_t), &cause_id };
                mdb_put(txn, hmem->dbi_idx_causal, &k, &v, MDB_APPENDDUP);
                // NB: в реальном коде используй отдельный dbi_idx_causal, а не idx_context
            }
        }

        cJSON *embed_json = cJSON_GetObjectItem(atom_item, "embedding");
        if (cJSON_IsArray(embed_json) && cJSON_GetArraySize(embed_json) == VECTOR_DIM) {
            Vector128 vec;
            for (int d = 0; d < VECTOR_DIM; d++) {
                vec.data[d] = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(embed_json, d));
            }
            hyper_vector_save(txn, db.graph.hyper.idx_vectors, atom.id, &vec);
        }

        if (proc_kind(atom.process_id) == PROC_KIND_GOAL) {
            wm_activate(&global_wm, atom.id, atom.sti, atom.valence);
        }
    }

    cJSON_Delete(root);
    return 0;
}
