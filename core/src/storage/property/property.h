// storage/property/property.h
#pragma once

#include <stdint.h>
#include <lmdb.h>
#include "types/id.h"

typedef enum {
    PROP_INT,
    PROP_FLOAT,
    PROP_STRING,
    PROP_VECTOR,
    PROP_BINARY,
    PROP_BOOL,
    PROP_NODE_REF
} PropertyType;

typedef struct {
    node_id_t node_id;
    uint64_t key_hash;
    PropertyType type;
    uint32_t size;
    // payload лежит в памяти сразу за этой структурой
} NodeProperty;

/*
 * Открытая сумка свойств произвольного узла/атома.
 * Ключ в LMDB: {node_id_t node_id; uint64_t key_hash = djb2_hash(key)}.
 * db.graph.properties НЕ DUPSORT — mdb_put(flags=0) перезаписывает
 * предыдущее значение (node_id,key) целиком: это "текущее значение",
 * а не append-only лог.
 */
int property_set(MDB_txn *txn, node_id_t node_id, const char *key,
                  PropertyType type, const void *payload, uint32_t size);

int property_get(MDB_txn *txn, node_id_t node_id, const char *key,
                  PropertyType *out_type, void *out_buf, uint32_t buf_size, uint32_t *out_size);
