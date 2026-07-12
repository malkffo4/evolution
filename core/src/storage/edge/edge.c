// edge.c
#include <string.h>

#include "storage/db/db.h"
#include "storage/edge/edge.h"

int create_edge(MDB_txn *txn, const Edge *edge) {
    MDB_val key = { sizeof(edge->triple), (void*)&edge->triple };
    MDB_val data = { sizeof(*edge), (void*)edge };
    int rc = mdb_put(txn, db.graph.edges, &key, &data, MDB_NOOVERWRITE);
    if (rc != MDB_SUCCESS)
        return rc;

    // индекс по источнику
    MDB_val src_key = { sizeof(node_id_t), (void*)&edge->triple.source };
    MDB_val src_data = { sizeof(Triple), (void*)&edge->triple };
    rc = mdb_put(txn, db.graph.index.edges_by_source, &src_key, &src_data, 0);
    if (rc != MDB_SUCCESS)
        return rc;

    // индекс по цели
    MDB_val tgt_key = { sizeof(node_id_t), (void*)&edge->triple.target };
    MDB_val tgt_data = { sizeof(Triple), (void*)&edge->triple };
    rc = mdb_put(txn, db.graph.index.edges_by_target, &tgt_key, &tgt_data, 0);
    return rc; // точка с запятой обязательна
}

int update_edge(MDB_txn *txn, const Edge *edge) {
    MDB_val key = {
        sizeof(edge->triple),
        (void *)&edge->triple
    };

    MDB_val data = {
        sizeof(*edge),
        (void *)edge
    };

    return mdb_put(txn, db.graph.edges, &key, &data, 0);
}

int get_edge(MDB_txn *txn, const Triple *triple, Edge *edge) {
    MDB_val key = {
        sizeof(*triple),
        (void *)triple
    };

    MDB_val data;

    int rc = mdb_get(txn, db.graph.edges, &key, &data);

    if (rc != MDB_SUCCESS)
        return rc;

    if (data.mv_size != sizeof(Edge))
        return MDB_BAD_VALSIZE;

    memcpy(edge, data.mv_data, sizeof(*edge));

    return MDB_SUCCESS;
}

int delete_edge(MDB_txn *txn, const Triple *triple) {
    MDB_val key = {
        sizeof(*triple),
        (void *)triple
    };

    return mdb_del(txn, db.graph.edges, &key, NULL);
}
