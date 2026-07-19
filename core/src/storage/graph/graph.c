// storage/graph.c
#include <stdint.h>
#include <lmdb.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>

#include "storage/db/db.h"
#include "storage/edge/edge.h"
#include "storage/graph/graph.h"
#include "storage/string_pool/string_pool.h"
#include "runtime/logging/logging.h"

// get_edges_from_node()
// get_edges_to_node()
// graph_neighbors()
// graph_bfs()
// graph_dfs()
// graph_shortest_path()

int upsert_edge(MDB_txn *txn, const Edge *new_edge) {
    Edge edge;

    int rc = get_edge(txn, &new_edge->key, &edge);

    if (rc == MDB_NOTFOUND) {
        Edge copy = *new_edge;

        copy.confidence = 0.4f;
        copy.evidence_count = 1;

        rc = create_edge(txn, &copy);
        if (rc != MDB_SUCCESS)
            return rc;

        LOG_GRAPH(
            "new edge %lu -( %lu )-> %lu confidence %.2f",
            copy.key.source,
            copy.key.relation,
            copy.key.target,
            copy.confidence);

        return MDB_SUCCESS;
    }

    if (rc != MDB_SUCCESS)
        return rc;

    edge.evidence_count++;

    edge.confidence +=
        (1.0f - edge.confidence) * 0.2f;

    rc = update_edge(txn, &edge);

    if (rc == MDB_SUCCESS) {
        LOG_GRAPH(
            "reinforced edge %lu -( %lu )-> %lu confidence %.2f evidence %u",
            edge.key.source,
            edge.key.relation,
            edge.key.target,
            edge.confidence,
            edge.evidence_count);
    }

    return rc;
}

int get_edges_from_node(MDB_txn *txn, node_id_t source, EdgeList *list) {
    MDB_cursor *cursor;
    MDB_val key, data;
    int rc;

    list->items = NULL;
    list->count = 0;

    rc = mdb_cursor_open(txn, db.graph.index.edges_by_source, &cursor);
    if (rc != MDB_SUCCESS) return rc;

    key.mv_size = sizeof(node_id_t);
    key.mv_data = (void *)&source;

    rc = mdb_cursor_get(cursor, &key, &data, MDB_SET);
    if (rc == MDB_NOTFOUND) {
        mdb_cursor_close(cursor);
        return MDB_SUCCESS;
    }
    if (rc != MDB_SUCCESS) {
        mdb_cursor_close(cursor);
        return rc;
    }

    /* Подсчитаем количество ребер */
    int count = 1;
    while (mdb_cursor_get(cursor, &key, &data, MDB_NEXT_DUP) == MDB_SUCCESS) {
        count++;
    }

    /* Выделим память */
    list->items = (Edge *)malloc(count * sizeof(Edge));
    if (!list->items) {
        mdb_cursor_close(cursor);
        return ENOMEM;
    }

    /* Заполним массив */
    rc = mdb_cursor_get(cursor, &key, &data, MDB_SET);
    int idx = 0;
    while (rc == MDB_SUCCESS && idx < count) {
        if (data.mv_size == sizeof(Triple)) {
            Triple triple;
            memcpy(&triple, data.mv_data, sizeof(Triple));
            rc = get_edge(txn, &triple, &list->items[idx]);
            if (rc == MDB_SUCCESS) {
                idx++;
            } else {
                // ошибка, прерываем, освобождаем память
                free(list->items);
                list->items = NULL;
                mdb_cursor_close(cursor);
                return rc;
            }
        }
        rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_DUP);
    }
    list->count = idx;
    mdb_cursor_close(cursor);
    return MDB_SUCCESS;
}

int get_edges_to_node(MDB_txn *txn, node_id_t target, EdgeList *list) {
    MDB_cursor *cursor;
    MDB_val key, data;
    int rc;

    list->items = NULL;
    list->count = 0;

    rc = mdb_cursor_open(txn, db.graph.index.edges_by_target, &cursor);
    if (rc != MDB_SUCCESS) return rc;

    key.mv_size = sizeof(node_id_t);
    key.mv_data = (void *)&target;

    rc = mdb_cursor_get(cursor, &key, &data, MDB_SET);
    if (rc == MDB_NOTFOUND) {
        mdb_cursor_close(cursor);
        return MDB_SUCCESS;
    }
    if (rc != MDB_SUCCESS) {
        mdb_cursor_close(cursor);
        return rc;
    }

    // подсчет
    uint32_t count = 0;
    MDB_val saved_key = key;
    rc = mdb_cursor_get(cursor, &key, &data, MDB_SET);
    while (rc == MDB_SUCCESS) {
        count++;
        rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_DUP);
    }
    // второй проход для заполнения
    list->items = malloc(count * sizeof(Edge));
    if (!list->items) {
        mdb_cursor_close(cursor);
        return ENOMEM;
    }

    // возвращаемся к началу
    rc = mdb_cursor_get(cursor, &saved_key, &data, MDB_SET);
    uint32_t idx = 0;
    while (rc == MDB_SUCCESS && idx < count) {
        if (data.mv_size == sizeof(Triple)) {
            Triple triple_key;
            memcpy(&triple_key, data.mv_data, sizeof(Triple));
            rc = get_edge(txn, &triple_key, &list->items[idx]);
            if (rc == MDB_SUCCESS) {
                idx++;
            } else {
                // ошибка, прерываем, освобождаем память
                free(list->items);
                list->items = NULL;
                mdb_cursor_close(cursor);
                return rc;
            }
        }
        rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_DUP);
    }

    list->count = idx;
    mdb_cursor_close(cursor);
    return MDB_SUCCESS;
}

void graph_connect(MDB_txn *txn, node_id_t source, node_id_t relation, node_id_t target, float confidence, uint32_t context) {
    Edge edge = {
        .key = {
            .source = source,
            .relation = relation,
            .target = target
        },
        .context = context,
        .confidence = confidence,
        .evidence_count = 1
    };

    upsert_edge(txn, &edge);
}
