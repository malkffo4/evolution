// perception/perception.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory/working.h"
#include "storage/db/db.h"
#include "storage/graph/graph.h"
#include "storage/string_pool/string_pool.h"
#include "lib/cJSON.h"
#include "math/hash.h"

// Загрузка знаний из JSON напрямую в Рабочую Память (Working Memory)
int perceive_and_activate(const char *json_str, WorkingMemory *wm, MDB_txn *txn) {
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
                logic_edge.confidence = 0.5f; // Стартовая гипотеза
                logic_edge.context = 0;
                upsert_edge(txn, &logic_edge);
            }
        }
    }

    cJSON_Delete(json);
    return 0;
}

// perceive()
    // perceive_and_activate()
