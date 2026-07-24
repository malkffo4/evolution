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
#include <cjson/cJSON.h>
#include "math/hash.h"
#include "runtime/logging/logging.h"
#include "runtime/operator/operator.h"

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

            float danger = (float)cJSON_IsNumber(danger_json) ? danger_json->valuedouble : 0.1f;
            float utility = (float)cJSON_IsNumber(utility_json) ? utility_json->valuedouble : 0.1f;

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
                printf("  [ВОСПРИЯТИЕ] Узел '%s' (Опасность: %.2f, Польза: %.2f)\n", label->valuestring, danger, utility);
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

                // Создаём гипер-атом EDGE
                HyperAtom edge_atom = {
                    .id = 0,  // будет присвоен в hyper_assert_unique
                    .process_id = djb2_hash("EDGE"),
                    .args = {
                        HYPER_MAKE_REF(logic_edge.key.source),
                        HYPER_MAKE_REF(logic_edge.key.relation),
                        HYPER_MAKE_REF(logic_edge.key.target)
                    },
                    .context_id = 0,
                    .time_tick = (uint64_t)time(NULL),
                    .cause_id = 0
                };
                static uint64_t next_edge_id = 10000;  // отдельный счётчик
                edge_atom.id = ++next_edge_id;
                hyper_assert_unique(hmem, &edge_atom);
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
        return (ko_id_t)(arg_item->valueint) | HYPER_TYPE_INT;
    }
    return 0; // неизвестный тип
}

int perceive_hyper_json(const char *json_str, MDB_txn *txn, HyperMemory *hmem) {
    if (!json_str || !hmem) return -1;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        LOG_ERROR("perceive_hyper_json: failed to parse JSON");
        return -1;
    }

    cJSON *atoms = cJSON_GetObjectItem(root, "atoms");
    if (!cJSON_IsArray(atoms)) {
        cJSON_Delete(root);
        LOG_ERROR("perceive_hyper_json: missing 'atoms' array");
        return -1;
    }

    cJSON *atom_item;
    cJSON_ArrayForEach(atom_item, atoms) {
        // Читаем process (обязательно)
        cJSON *process_json = cJSON_GetObjectItem(atom_item, "process");
        if (!cJSON_IsString(process_json)) continue;
        ko_id_t process_id = djb2_hash(process_json->valuestring);

        // Аргументы (обязательно)
        cJSON *args_json = cJSON_GetObjectItem(atom_item, "args");
        if (!cJSON_IsArray(args_json)) continue;

        HyperAtom atom = {0};
        atom.id = 0; // будет сгенерирован позже (пока можно автоинкремент из хелпера)
        atom.process_id = process_id;

        int arg_count = cJSON_GetArraySize(args_json);
        for (int i = 0; i < arg_count && i < 3; i++) {
            cJSON *arg = cJSON_GetArrayItem(args_json, i);
            atom.args[i].raw = resolve_arg(arg);
        }
        // Если аргументов больше 3 – используем продолжение (но пока не реализовано)

        // Контекст (опционально)
        cJSON *ctx_json = cJSON_GetObjectItem(atom_item, "context");
        atom.context_id = ctx_json ? (ko_id_t)cJSON_GetNumberValue(ctx_json) : 0;

        // Время (опционально, иначе текущее)
        cJSON *time_json = cJSON_GetObjectItem(atom_item, "time");
        atom.time_tick = time_json ? (uint64_t)cJSON_GetNumberValue(time_json) : (uint64_t)time(NULL);

        // Причина (опционально)
        cJSON *cause_json = cJSON_GetObjectItem(atom_item, "cause");
        if (cJSON_IsString(cause_json)) {
            atom.cause_id = djb2_hash(cause_json->valuestring);
        } else if (cJSON_IsNumber(cause_json)) {
            atom.cause_id = (ko_id_t)cJSON_GetNumberValue(cause_json);
        } else {
            atom.cause_id = 0;
        }

        // Уверенность (опционально, сохраним отдельным атомом BELIEF, если нужно)
        cJSON *conf_json = cJSON_GetObjectItem(atom_item, "confidence");
        if (conf_json && cJSON_IsNumber(conf_json)) {
            float conf = (float)conf_json->valuedouble;
            // Создаём атом BELIEF: (atom.id, ID_BELIEF, atom.id, conf)
            // Но atom.id ещё не известен – сначала сохраним атом, потом добавим оценку
            // Упростим: запомним, что нужно создать BELIEF после
        }

        // Генерация ID
        static uint64_t next_id = 0;
        atom.id = ++next_id; // временное решение, потом заменим на глобальный счетчик

        int result = hyper_assert_unique(hmem, &atom);
        if (result != 0 && result != 1) {   // <-- разрешаем уже существующие атомы
            LOG_ERROR("Failed to assert atom");
        } else {
            LOG_DEBUG("Asserted atom: id=%lu process=%lu", atom.id, atom.process_id);
            if (atom.process_id == djb2_hash("HAS_ALGORITHM")) {
                ko_id_t goal = HYPER_GET_ID(atom.args[0].raw);
                ko_id_t algo = HYPER_GET_ID(atom.args[1].raw);
                if (global_wm_ptr) {
                    wm_activate(global_wm_ptr, goal, 0.8f, 0.9f);   // цель
                    wm_activate(global_wm_ptr, algo, 0.5f, 0.5f);   // алгоритм (чтобы был в WM, но с меньшим приоритетом)
                }
            }
            // Если была confidence, создаём атом BELIEF
            if (conf_json && cJSON_IsNumber(conf_json)) {
                HyperAtom belief_atom = {0};
                belief_atom.id = ++next_id;
                belief_atom.process_id = 0x0002; // ID_BELIEF (должно быть определено)
                belief_atom.args[0].raw = HYPER_MAKE_REF(atom.id);
                union { float f; uint32_t i; } u;
                u.f = (float)conf_json->valuedouble;
                belief_atom.args[1].raw = (ko_id_t)u.i | HYPER_TYPE_FLOAT;
                hyper_assert_unique(hmem, &belief_atom);
            }
        }
    }

    cJSON_Delete(root);
    return 0;
}
