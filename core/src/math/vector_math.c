// math/vector_math.c
#include <stdint.h>
#include <math.h>

#include "vector_math.h"
// Поиск смысловой разницы. Меньше число = ближе смысл.
// 0 = полные синонимы. > 30 = вообще разные вещи.
int semantic_distance_u64(uint64_t hash_A, uint64_t hash_B) {
    // XOR оставляет единицы только там, где биты различаются.
    uint64_t diff = hash_A ^ hash_B;
    // Встроенная ассемблерная команда процессора (очень быстрая), считает количество единиц.
    return __builtin_popcountll(diff);
}

float vector_cosine_similarity(const Vector128 *a, const Vector128 *b) {
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (int i = 0; i < VECTOR_DIM; i++) {
        dot   += a->data[i] * b->data[i];
        norm_a += a->data[i] * a->data[i];
        norm_b += b->data[i] * b->data[i];
    }
    if (norm_a < 1e-8f || norm_b < 1e-8f) return 0.0f;
    return dot / (sqrtf(norm_a) * sqrtf(norm_b));
}
