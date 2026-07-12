#ifndef EDGE_H
#define EDGE_H

#include <lmdb.h>

#include "storage/node/node.h"

typedef struct {
    node_id_t source;
    node_id_t relation;
    node_id_t target;
} Triple; // EdgeKey

typedef struct {
    Triple key;
    context_id_t context;
    episode_id_t episode;
    node_id_t author;
    node_id_t evidence;
    float confidence; // насколько знание считается истинным
    float activation; // насколько оно сейчас активно
    uint32_t evidence_count;
    uint32_t access_count; // сколько раз использовалось
    uint16_t flags;
    uint64_t created_at;
    uint64_t updated_at;
    uint64_t last_access;
    uint32_t success_count;
    uint32_t failure_count;
} Edge;

int create_edge(MDB_txn *txn, const Edge *edge);
int update_edge(MDB_txn *txn, const Edge *edge);
int get_edge(MDB_txn *txn, const Triple *key, Edge *edge);
int delete_edge(MDB_txn *txn, const Triple *key);

#endif // EDGE_H
