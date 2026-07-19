// knowledge/knowledge_cache.c
#include <stdint.h>
#include <stdlib.h>

#include "storage/graph/graph.h"
#include "runtime/vm/vm_context.h"

// Загружает все исходящие рёбра узла с заданным отношением в кеш VM
int knowledge_cache_load_edges(VMContext *ctx, MDB_txn *txn, node_id_t source, node_id_t relation) {
    EdgeList list = {0};
    // Используем существующие функции графа для получения рёбер
    int rc = get_edges_from_node(txn, source, &list);
    if (rc != MDB_SUCCESS) return rc;

    ctx->preloaded_edge_count = 0;
    for (uint32_t i = 0; i < list.count && ctx->preloaded_edge_count < MAX_PRELOADED_EDGES; i++) {
        if (list.items[i].key.relation == relation) {
            ctx->preloaded_edges[ctx->preloaded_edge_count++] = (CachedEdge){
                .source   = list.items[i].key.source,
                .target   = list.items[i].key.target,
                .relation = list.items[i].key.relation
            };
        }
    }
    free(list.items);
    return MDB_SUCCESS;
}
