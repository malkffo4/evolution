// knowledge/knowledge_cache.c
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "storage/db/db.h"
#include "storage/graph/graph.h"
// #include "storage/property.h"
#include "runtime/vm/vm_context.h"
#include "storage/vector_store/vector_store.h"
#include "runtime/logging/logging.h"

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
    struct { node_id_t nid; uint64_t hash; } db_key;
    MDB_val key, data;
    int rc;

    if (db.graph.properties == 0) {
        LOG_ERROR("[CACHE] properties DBI not opened!");
        return MDB_NOTFOUND;
    }

    MDB_cursor *cursor;
    rc = mdb_cursor_open(txn, db.graph.properties, &cursor);
    if (rc != MDB_SUCCESS) return rc;

    db_key.nid = node_id;
    db_key.hash = 0;
    key.mv_size = sizeof(db_key);
    key.mv_data = &db_key;

    rc = mdb_cursor_get(cursor, &key, &data, MDB_SET_RANGE);
    while (rc == MDB_SUCCESS && ctx->preloaded_property_count < MAX_PRELOADED_PROPERTIES) {
        // Копируем ключ в локальную структуру (безопасно)
        struct { node_id_t nid; uint64_t hash; } entry;
        if (key.mv_size < sizeof(entry)) {
            rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
            continue;
        }
        memcpy(&entry, key.mv_data, sizeof(entry));
        if (entry.nid != node_id) break;

        if (data.mv_size < sizeof(NodeProperty)) {
            rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
            continue;
        }

        // Копируем заголовок (безопасно, без выравнивания)
        NodeProperty header;
        memcpy(&header, data.mv_data, sizeof(NodeProperty));

        if (data.mv_size < sizeof(NodeProperty) + header.size) {
            rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
            continue;
        }

        const void *payload = (const char *)data.mv_data + sizeof(NodeProperty);

        CachedProperty *cp = &ctx->preloaded_properties[ctx->preloaded_property_count];
        memset(cp, 0, sizeof(CachedProperty));  // <-- ОБЯЗАТЕЛЬНО обнуляем!
        cp->node_id = node_id;
        cp->key_hash = entry.hash;
        cp->type = header.type;

        // Копируем значение в зависимости от типа
        switch (header.type) {
            case PROP_INT:
                if (header.size == sizeof(int)) {
                    int val;
                    memcpy(&val, payload, sizeof(int));
                    cp->value.i = val;
                    LOG_DEBUG("[CACHE] Loaded int property: node=%lu, key_hash=%lu, value=%d",
                              node_id, entry.hash, val);
                } else {
                    rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
                    continue;
                }
                break;
            case PROP_FLOAT:
                if (header.size == sizeof(float)) {
                    float val;
                    memcpy(&val, payload, sizeof(float));
                    cp->value.f = val;
                } else {
                    rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
                    continue;
                }
                break;
            case PROP_BOOL:
                if (header.size == sizeof(bool)) {
                    bool val;
                    memcpy(&val, payload, sizeof(bool));
                    cp->value.b = val;
                } else {
                    rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
                    continue;
                }
                break;
            default:
                rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
                continue;
        }

        ctx->preloaded_property_count++;
        rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
    }

    mdb_cursor_close(cursor);
    return MDB_SUCCESS;
}

int knowledge_cache_load_embeddings(VMContext *ctx, MDB_txn *txn, node_id_t node_id) {
    float emb[VECTOR_DIM];
    int rc = load_embedding(txn, node_id, emb);
    if (rc != 0) return rc;// Ищем свободный слот в верхней половине scratchpad (индексы 32..63)

    // Ищем свободный слот в верхней половине scratchpad (индексы 32..63)
    for (uint32_t i = 32; i < MAX_SCRATCHPAD; i++) {
        if (ctx->scratchpad[i].value == 0) {   // слот свободен
            float *copy = malloc(VECTOR_DIM * sizeof(float));
            if (!copy) return ENOMEM;
            memcpy(copy, emb, VECTOR_DIM * sizeof(float));

            ctx->scratchpad[i].key_hash = node_id;
            ctx->scratchpad[i].value = (int64_t)(uintptr_t)copy;
            return MDB_SUCCESS;
        }
    }
    return ENOSPC;
}
