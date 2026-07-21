// свойства узлов/ребер
// storage/property.h
#ifndef PROPERTY_H
#define PROPERTY_H

typedef enum {
    PROP_INT,
    PROP_FLOAT,
    PROP_STRING,
    PROP_VECTOR,
    PROP_BINARY,
    PROP_BOOL,
    PROP_NODE_REF
} PropertyType;

// СВОЙСТВА. Хранилище массивов и сырого текста для узла.
typedef struct {
    node_id_t node_id;
    uint64_t key_hash;
    PropertyType type;     // 0 = INT, 1 = STRING, 2 = FLOAT_ARRAY (Embeddings)
    uint32_t size;          // Размер данных payload
    // payload лежит в памяти сразу за этой структурой
} NodeProperty;

#endif // PROPERTY_H
