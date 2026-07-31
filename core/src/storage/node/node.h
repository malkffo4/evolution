// storage/node/node.h
#pragma once

#include <lmdb.h>
#include <stdint.h>

#include "types/id.h"

// УЗЕЛ. Теперь он хранит не только ID, но и свой семантический смысл.
typedef struct {
    node_id_t id;
    node_id_t simhash;
    node_id_t name_hash;   // Строгий хэш (djb2) для точного поиска
    node_id_t type;        // новый тип ребра IS_A (или INSTANCE_OF). Значения: ENTITY, EVENT, EPISODE, RULE, HYPOTHESIS.
                          // если узел X IS_A Y, а Y имеет свойство Z, то X тоже имеет свойство Z (наследование).
} Node;

/* Узлы */
int create_node(MDB_txn *txn, const Node *node);
int get_node(MDB_txn *txn, node_id_t id, Node *node);
int delete_node(MDB_txn *txn, node_id_t id);
