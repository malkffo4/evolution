// storage/node/node.c
#include <lmdb.h>
#include <string.h>
#include <stddef.h>

#include "storage/db/db.h"
#include "storage/node/node.h"

/* Узлы */
int create_node(MDB_txn *txn, const Node *node) {
    MDB_val key, data;

    key.mv_size = sizeof(node_id_t);
    key.mv_data = (void *)&node->id;

    data.mv_size = sizeof(Node);
    data.mv_data = (void *)node;

    return mdb_put(txn, db.graph.nodes, &key, &data, 0);
}

int get_node(MDB_txn *txn, node_id_t id, Node *node) {
    MDB_val key, data;
    int rc;

    key.mv_size = sizeof(node_id_t);
    key.mv_data = (void *)&id;

    rc = mdb_get(txn, db.graph.nodes, &key, &data);
    if (rc != MDB_SUCCESS) {
        return rc;
    }

    memcpy(node, data.mv_data, sizeof(Node));
    return MDB_SUCCESS;
}

int delete_node(MDB_txn *txn, node_id_t id) {
    MDB_val key;

    key.mv_size = sizeof(node_id_t);
    key.mv_data = (void *)&id;

    return mdb_del(txn, db.graph.nodes, &key, NULL);
}
