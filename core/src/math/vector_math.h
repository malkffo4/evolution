// math/vector_math.h
#pragma once

#include <stdint.h>

#define VECTOR_DIM 128

typedef struct {
    float data[VECTOR_DIM];
} Vector128;

int semantic_distance_u64(uint64_t hash_A, uint64_t hash_B);
float vector_cosine_similarity(const Vector128 *a, const Vector128 *b);
