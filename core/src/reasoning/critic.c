// reasoning/critic.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/db/db.h"
#include "storage/node/node.h"
#include "storage/edge/edge.h"
#include "storage/graph/graph.h"
#include "storage/string_pool/string_pool.h"
#include "math/hash.h"

void analyze_error(const char *error_log, uint64_t algo_id, MDB_txn *txn) {
    // 1. Найти все связи, которые ВЕДУТ к этому algo_id
    //    (т.е. ребра, где algo_id является целью)
    (void)error_log;
    EdgeList edges_to_algo;
    if (get_edges_to_node(txn, algo_id, &edges_to_algo) == MDB_SUCCESS) {
        for (uint32_t i = 0; i < edges_to_algo.count; i++) {
            // 2. Понизить уверенность в этих связях
            edges_to_algo.items[i].confidence *= 0.8f; // Уменьшаем на 20%
            update_edge(txn, &edges_to_algo.items[i]);

            // 3. Если алгоритм окончательно сломался, пометить его
            if (edges_to_algo.items[i].confidence < 0.1f) {
                // Создаем связь "алгоритм имеет дефект"
                uint64_t flaw_rel = djb2_hash("HAS_FLAW");
                Edge flaw = { .key = { algo_id, flaw_rel, algo_id }, .confidence = 1.0f };
                create_edge(txn, &flaw);
            }
        }
        free(edges_to_algo.items);
    }
}
