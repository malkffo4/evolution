#ifndef SEMANTIC_H
#define SEMANTIC_H

typedef struct {
    uint64_t bits[4];
} simhash256_t; // Новая сущность: битовая карта смысла

typedef struct {
    node_id_t node;
    simhash256_t signature;
} SemanticIndex;

#endif // SEMANTIC_
