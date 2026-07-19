// RAG (Retrieval-Augmented Generation)
// knowledge/rag.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/graph/graph.h"
#include "storage/string_pool/string_pool.h"
#include "lib/cJSON.h"
#include "types/id.h"
#include "runtime/logging/logging.h"

// Извлекает семантический подграф (RAG Context) вокруг заданного узла
// Использует BFS (поиск в ширину) для сбора соседних понятий
char* retrieve_subgraph_json(MDB_txn *txn, node_id_t start_node, int max_depth, int max_nodes) {
    cJSON *root = cJSON_CreateObject();
    cJSON *nodes_arr = cJSON_AddArrayToObject(root, "nodes");
    cJSON *edges_arr = cJSON_AddArrayToObject(root, "edges");

    node_id_t *queue = malloc((size_t)max_nodes * sizeof(node_id_t));
    node_id_t *visited = malloc((size_t)max_nodes * sizeof(node_id_t));
    if (!queue || !visited) {
        if(queue) free(queue);
        if(visited) free(visited);
        cJSON_Delete(root);
        return NULL;
    }

    int q_head = 0, q_tail = 0, v_count = 0;
    queue[q_tail++] = start_node;
    visited[v_count++] = start_node;

    int current_depth = 0;
    int nodes_current = 1, nodes_next = 0;

    LOG_GRAPH("Starting RAG subgraph extraction for node %lu", start_node);

    while (q_head < q_tail && v_count < max_nodes && current_depth <= max_depth) {
        node_id_t curr = queue[q_head++];
        nodes_current--;

        // 1. Добавляем текущий узел в JSON контекст
        const char *name = get_string_from_pool(txn, curr);
        cJSON *n_json = cJSON_CreateObject();
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%lu", curr);
        cJSON_AddStringToObject(n_json, "id", id_str);
        if (name) {
            cJSON_AddStringToObject(n_json, "label", name);
        }
        cJSON_AddItemToArray(nodes_arr, n_json);

        // 2. Получаем ИСХОДЯЩИЕ рёбра
        EdgeList out_edges = {0};
        if (get_edges_from_node(txn, curr, &out_edges) == MDB_SUCCESS && out_edges.items) {
            for (uint32_t i = 0; i < out_edges.count; i++) {
                node_id_t tgt = out_edges.items[i].key.target;
                const char *rel = get_string_from_pool(txn, out_edges.items[i].key.relation);

                cJSON *e_json = cJSON_CreateObject();
                char tgt_str[32];
                snprintf(tgt_str, sizeof(tgt_str), "%lu", tgt);
                cJSON_AddStringToObject(e_json, "source", id_str);
                cJSON_AddStringToObject(e_json, "target", tgt_str);
                if (rel) cJSON_AddStringToObject(e_json, "relation", rel);
                cJSON_AddItemToArray(edges_arr, e_json);

                // Добавляем соседей в очередь
                int found = 0;
                for (int v = 0; v < v_count; v++) {
                    if (visited[v] == tgt) { found = 1; break; }
                }
                if (!found && v_count < max_nodes) {
                    visited[v_count++] = tgt;
                    queue[q_tail++] = tgt;
                    nodes_next++;
                }
            }
            free(out_edges.items);
        }

        // Обрабатываем переход на следующий уровень глубины графа
        if (nodes_current == 0) {
            current_depth++;
            nodes_current = nodes_next;
            nodes_next = 0;
        }
    }

    free(queue);
    free(visited);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

// Файлы: evolution/core/src/reasoning/analogy.c и evolution/core/src/knowledge/retrieval.c
// TODO Задачи:
// Реализуйте поиск по вектору (vector_store.c).
// Если система видит структуру атаки "SQL Injection",
// она должна уметь через функцию find_similar_structures() собрать похожий граф для "NoSQL Injection",
// скомпилировать его и отдать в примитивную VM на проверку.
