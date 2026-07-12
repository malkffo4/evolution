// source-relation-target

#ifndef GRAPH_H
#define GRAPH_H

#include <lmdb.h>
#include <stdint.h>

#include "types/id.h"
#include "storage/node/node.h"
#include "storage/edge/edge.h"

typedef struct {
    node_id_t *items;
    uint32_t count;
    uint32_t capacity;
} NodeList;

/* Поиск ребер */
typedef struct {
    Edge *items;
    uint32_t count;
    uint32_t capacity;
} EdgeList;

typedef struct {
    // node_id_t *items;
    // Edge *edges;
    // uint32_t node_count;
    // uint32_t edge_count;

    NodeList nodes;
    EdgeList edges;
} GraphView;

int upsert_edge(MDB_txn *txn, const Edge *new_edge);
void graph_connect(MDB_txn *txn, node_id_t source, node_id_t relation, node_id_t target, float confidence, uint32_t context);
int get_edges_from_node(MDB_txn *txn, node_id_t source, EdgeList *list);
int get_edges_to_node(MDB_txn *txn, node_id_t target, EdgeList *list);

// graph_add_relation()
// graph_remove_relation()
// graph_find_relation()
// graph_neighbors()
// graph_outgoing()
// graph_incoming()
// void edge_list_init(EdgeList *list);
// void edge_list_free(EdgeList *list);
// void node_list_init(NodeList *list);
// void node_list_free(NodeList *list);
// void graph_view_init(GraphView *graph);
// void graph_view_free(GraphView *graph);

#endif // GRAPH_H
