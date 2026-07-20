// knowledge/knowledge_cache.c
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "storage/graph/graph.h"
#include "runtime/vm/vm_context.h"
#include "storage/vector_store/vector_store.h"

// Загружает все исходящие рёбра узла с заданным отношением в кеш VM
int knowledge_cache_load_edges(VMContext *ctx, MDB_txn *txn, node_id_t source, node_id_t relation) {
    EdgeList list = {0};
    // Используем существующие функции графа для получения рёбер
    int rc = get_edges_from_node(txn, source, &list);
    if (rc != MDB_SUCCESS) return rc;

    // НЕ сбрасываем preloaded_edge_count, добавляем новые рёбра поверх уже загруженных
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

int knowledge_cache_load_properties(VMContext *ctx, MDB_txn *txn, node_id_t node_id) {
    (void)txn;
    // Пока заглушка — свойства не нужны для теста гипотез
    (void)ctx;
    (void)node_id;
    return MDB_SUCCESS;
}

int knowledge_cache_load_embeddings(VMContext *ctx, MDB_txn *txn, node_id_t node_id) {
    float emb[EMBEDDING_DIM];
    int rc = load_embedding(txn, node_id, emb);
    if (rc != 0) return rc;

    // Ищем свободный слот в верхней половине scratchpad (индексы 32..63)
    for (uint32_t i = 32; i < MAX_SCRATCHPAD; i++) {
        if (ctx->scratchpad[i].value == 0) {   // слот свободен
            float *copy = malloc(EMBEDDING_DIM * sizeof(float));
            if (!copy) return ENOMEM;
            memcpy(copy, emb, EMBEDDING_DIM * sizeof(float));
            ctx->scratchpad[i].key_hash = node_id;
            ctx->scratchpad[i].value = (int64_t)(uintptr_t)copy;
            return MDB_SUCCESS;
        }
    }
    return ENOSPC;
}
