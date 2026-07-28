// knowledge/hyper_retrieval.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hyper_retrieval.h"
#include <cjson/cJSON.h>
#include "runtime/logging/logging.h"
#include "storage/node/node.h"
#include "storage/string_pool/string_pool.h"

static void resolve_label(MDB_txn *txn, ko_id_t id, char *out, size_t out_size) {
    Node n;
    if (get_node(txn, id, &n) == MDB_SUCCESS) {
        const char *label = get_string_from_pool(txn, n.name_hash);
        if (label) {
            snprintf(out, out_size, "%s", label);
            return;
        }
    }
    // Это не узел (например, ID отношения relation или процесса EDGE) —
    // пробуем найти строку напрямую в string_pool по её собственному хэшу.
    const char *pooled = get_string_from_pool(txn, id);
    if (pooled) {
        snprintf(out, out_size, "%s", pooled);
        return;
    }
    snprintf(out, out_size, "0x%llx", (unsigned long long)id);
}

char* hyper_retrieve_json(HyperMemory *hmem, node_id_t participant_id, int max_depth, int max_atoms) {
    cJSON *root = cJSON_CreateObject();
    cJSON *atoms_arr = cJSON_AddArrayToObject(root, "atoms");

    // Очередь для BFS: массив participant_id для следующего уровня
    node_id_t *queue = malloc((size_t)max_atoms * sizeof(node_id_t));
    node_id_t *visited = malloc((size_t)max_atoms * sizeof(node_id_t));
    if (!queue || !visited) {
        if(queue) free(queue); if(visited) free(visited);
        cJSON_Delete(root); return NULL;
    }

    int q_head = 0, q_tail = 0, v_count = 0;
    queue[q_tail++] = participant_id;
    visited[v_count++] = participant_id;

    int current_depth = 0;
    int nodes_current = 1, nodes_next = 0;

    while (q_head < q_tail && v_count < max_atoms && current_depth <= max_depth) {
        node_id_t current_participant = queue[q_head++];
        nodes_current--;

        // Ищем все атомы, где current_participant является участником
        HyperAtom *results = NULL;
        size_t count = 0;
        if (hyper_find_by_participant(hmem, current_participant, 0, &results, &count) == 0) {
            for (size_t i = 0; i < count && (int)(q_tail + 1) < max_atoms; i++) {
                // Добавляем атом в результат
                cJSON *atom_json = cJSON_CreateObject();
                char idbuf[32];
                snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)results[i].id);
                cJSON_AddStringToObject(atom_json, "id", idbuf);

                const char *proc_label = get_string_from_pool(hmem->txn, results[i].process_id);
                cJSON_AddStringToObject(atom_json, "process", proc_label ? proc_label : "UNKNOWN");

                cJSON *args_json = cJSON_AddArrayToObject(atom_json, "args");
                for (int a = 0; a < 3; a++) {
                    if (results[i].args[a].raw != 0) {
                        char label[128];
                        if (HYPER_GET_TYPE(results[i].args[a].raw) == HYPER_TYPE_REF) {
                            resolve_label(hmem->txn, HYPER_GET_ID(results[i].args[a].raw), label, sizeof(label));
                        } else {
                            snprintf(label, sizeof(label), "%lld", (long long)results[i].args[a].raw);
                        }
                        cJSON_AddItemToArray(args_json, cJSON_CreateString(label));
                    }
                }
                snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)results[i].context_id);
                cJSON_AddStringToObject(atom_json, "context", idbuf);

                snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)results[i].time_tick);
                cJSON_AddStringToObject(atom_json, "time", idbuf);

                snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)results[i].cause_id);
                cJSON_AddStringToObject(atom_json, "cause", idbuf);

                cJSON_AddItemToArray(atoms_arr, atom_json);

                // Добавляем новых участников в очередь
                for (int a = 0; a < 3; a++) {
                    if (HYPER_GET_TYPE(results[i].args[a].raw) == HYPER_TYPE_REF) {
                        node_id_t new_participant = HYPER_GET_ID(results[i].args[a].raw);
                        int found = 0;
                        for (int v = 0; v < v_count; v++) {
                            if (visited[v] == new_participant) { found = 1; break; }
                        }
                        if (!found && v_count < max_atoms) {
                            visited[v_count++] = new_participant;
                            queue[q_tail++] = new_participant;
                            nodes_next++;
                        }
                    }
                }
            }
            free(results);
        }

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
