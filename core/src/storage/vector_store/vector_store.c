// storage/vector_store/vector_store.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <lmdb.h>

#include "storage/db/db.h"
#include "vector_store.h"
#include "runtime/logging/logging.h"

// =========================================================================
// Xoshiro256** — качественный PRNG с периодом 2^256-1
// =========================================================================
static uint64_t xs_state[4];

static inline uint64_t xs_rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t xs_next(void) {
    uint64_t result = xs_rotl(xs_state[1] * 5, 7) * 9;
    uint64_t t = xs_state[1] << 17;

    xs_state[2] ^= xs_state[0];
    xs_state[3] ^= xs_state[1];
    xs_state[1] ^= xs_state[2];
    xs_state[0] ^= xs_state[3];

    xs_state[2] ^= t;
    xs_state[3] = xs_rotl(xs_state[3], 45);

    return result;
}

static void xs_seed(uint64_t seed) {
    // SplitMix64 для инициализации состояния
    for (int i = 0; i < 4; i++) {
        uint64_t z = (seed += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        xs_state[i] = z ^ (z >> 31);
    }
}

// Генерация float в диапазоне [-1, 1) с равномерным распределением
static inline float xs_float(void) {
    // Берём старшие 23 бита для мантиссы float (наиболее качественные биты Xoshiro)
    uint32_t bits = (uint32_t)(xs_next() >> 40);
    // Преобразуем в float в диапазоне [0, 1)
    float f = (float)bits / (float)(1ULL << 23);
    // Масштабируем до [-1, 1)
    return f * 2.0f - 1.0f;
}

// =========================================================================
// Основной код
// =========================================================================

const char *config_key = "proj_matrix";
static float proj_matrix[HASH_BITS][EMBEDDING_DIM];

int init_simhash(MDB_txn *txn) {
    MDB_val key, data;
    int rc;

    // Try to load projection matrix from DB
    key.mv_size = strlen(config_key) + 1;
    key.mv_data = (void*)config_key;
    rc = mdb_get(txn, db.vectors.simhash_config, &key, &data);

    if (rc == MDB_SUCCESS) {
        if (data.mv_size != sizeof(proj_matrix)) {
            LOG_ERROR("Invalid projection matrix size in DB");
            return -1;
        }
        memcpy(proj_matrix, data.mv_data, sizeof(proj_matrix));
        LOG_DATABASE("Loaded projection matrix from DB");
        return 0;
    }

    // Matrix doesn't exist, generate new one
    LOG_DATABASE("Generating new projection matrix with Xoshiro256**...");
    xs_seed(42); // Детерминированная инициализация

    for (int i = 0; i < HASH_BITS; i++) {
        for (int j = 0; j < EMBEDDING_DIM; j++) {
            proj_matrix[i][j] = xs_float();
        }
    }

    // Save to DB
    data.mv_size = sizeof(proj_matrix);
    data.mv_data = proj_matrix;
    rc = mdb_put(txn, db.vectors.simhash_config, &key, &data, 0);
    if (rc != MDB_SUCCESS) {
        LOG_ERROR("Failed to save projection matrix: %s\n", mdb_strerror(rc));
        return rc;
    }

    return 0;
}

void compute_simhash256(const float *embedding, uint64_t hash[4]) {
    memset(hash, 0, 4 * sizeof(uint64_t));
    for (int i = 0; i < HASH_BITS; i++) {
        float sum = 0.0f;
        for (int j = 0; j < EMBEDDING_DIM; j++) {
            sum += embedding[j] * proj_matrix[i][j];
        }
        if (sum > 0) {
            hash[i / 64] |= (1ULL << (i % 64));
        }
    }
}

int save_embedding(MDB_txn *txn, uint64_t node_id, const float *emb) {
    MDB_val key, data;
    int rc;

    // Save embedding
    key.mv_size = sizeof(uint64_t);
    key.mv_data = &node_id;
    data.mv_size = EMBEDDING_DIM * sizeof(float);
    data.mv_data = (void *)emb;

    rc = mdb_put(txn, db.vectors.embeddings, &key, &data, 0);
    if (rc != MDB_SUCCESS) {
        return rc;
    }

    // Compute and save SimHash
    uint64_t simhash[4];
    compute_simhash256(emb, simhash);

    // Store in SimHash index (simhash -> node_id)
    key.mv_size = sizeof(simhash);
    key.mv_data = simhash;
    data.mv_size = sizeof(uint64_t);
    data.mv_data = &node_id;

    return mdb_put(txn, db.vectors.simhash_index, &key, &data, MDB_APPENDDUP);
}

int load_embedding(MDB_txn *txn, uint64_t node_id, float *emb_out) {
    MDB_val key, data;
    int rc;

    key.mv_size = sizeof(uint64_t);
    key.mv_data = &node_id;

    rc = mdb_get(txn, db.vectors.embeddings, &key, &data);
    if (rc != MDB_SUCCESS) {
        return rc;
    }

    if (data.mv_size != EMBEDDING_DIM * sizeof(float)) {
        return -1;
    }

    memcpy(emb_out, data.mv_data, data.mv_size);
    return 0;
}

// Helper function to compute cosine similarity
static float cosine_similarity(const float *a, const float *b, int dim) {
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;

    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    if (norm_a == 0 || norm_b == 0) return 0.0f;
    return dot / (sqrtf(norm_a) * sqrtf(norm_b));
}

// Helper function to count differing bits between two SimHashes
static int hamming_distance(const uint64_t *a, const uint64_t *b) {
    int dist = 0;
    for (int i = 0; i < 4; i++) {
        dist += __builtin_popcountll(a[i] ^ b[i]);
    }
    return dist;
}

int find_similar_nodes(MDB_txn *txn, const float *query_emb, int topK, uint64_t *results) {
    // First compute query's SimHash
    uint64_t query_hash[4];
    compute_simhash256(query_emb, query_hash);

    // Find candidate nodes with similar SimHash (Hamming distance < threshold)
    MDB_cursor *cursor;
    MDB_val key, data;
    int rc;

    rc = mdb_cursor_open(txn, db.vectors.simhash_index, &cursor);
    if (rc != MDB_SUCCESS) {
        return rc;
    }

    // We'll collect candidates with Hamming distance < 30 (configurable)
    uint64_t candidates[MAX_CANDIDATES]; // Max candidates
    int candidate_count = 0;

    // Iterate through all SimHash entries
    rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
    while (rc == MDB_SUCCESS && candidate_count < MAX_CANDIDATES) {
        if (key.mv_size == sizeof(query_hash)) {
            int dist = hamming_distance((uint64_t *)key.mv_data, query_hash);
            if (dist < HAMMING_THRESHOLD) {
                // This is a candidate, add all node_ids for this SimHash
                do {
                    if (data.mv_size == sizeof(uint64_t)) {
                        candidates[candidate_count++] = *(uint64_t *)data.mv_data;
                    }
                    rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_DUP);
                } while (rc == MDB_SUCCESS && candidate_count < MAX_CANDIDATES);
                rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_NODUP);
            } else {
                rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_NODUP);
            }
        } else {
            rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
        }
    }

    mdb_cursor_close(cursor);

    if (candidate_count == 0) {
        return 0; // No candidates found
    }

    // Now compute exact cosine similarity for candidates
    typedef struct {
        uint64_t node_id;
        float similarity;
    } SimilarityResult;

    SimilarityResult *similarities = malloc((size_t)candidate_count * sizeof(SimilarityResult));
    if (!similarities) {
        return -1;
    }

    int valid_count = 0;
    for (int i = 0; i < candidate_count; i++) {
        float emb[EMBEDDING_DIM];
        if (load_embedding(txn, candidates[i], emb) == 0) {
            similarities[valid_count].node_id = candidates[i];
            similarities[valid_count].similarity = cosine_similarity(query_emb, emb, EMBEDDING_DIM);
            valid_count++;
        }
    }

    if (valid_count == 0) {
        free(similarities);
        return 0;
    }

    // ---- Замена: Min-Heap для выбора topK (O(N log K)) ----
    // Куча хранит не более topK элементов, минимальная similarity – в корне.
    int heap_capacity = (topK < valid_count) ? topK : valid_count;
    SimilarityResult *heap = malloc((size_t)heap_capacity * sizeof(SimilarityResult));
    if (!heap) {
        free(similarities);
        return -1;
    }
    int heap_size = 0;

    // Вспомогательные макросы для работы с кучей (индексация с 0)
    #define PARENT(i) (((i) - 1) / 2)
    #define LEFT(i)   (2 * (i) + 1)
    #define RIGHT(i)  (2 * (i) + 2)

    // Добавление элемента в min-heap (просеивание вверх)
    for (int i = 0; i < valid_count; i++) {
        if (heap_size < heap_capacity) {
            // Место есть – добавляем в конец и просеиваем вверх
            int idx = heap_size++;
            heap[idx] = similarities[i];
            // Sift-up
            while (idx > 0) {
                int parent = PARENT(idx);
                if (heap[idx].similarity < heap[parent].similarity) {
                    SimilarityResult tmp = heap[idx];
                    heap[idx] = heap[parent];
                    heap[parent] = tmp;
                    idx = parent;
                } else {
                    break;
                }
            }
        } else if (similarities[i].similarity > heap[0].similarity) {
            // Новый элемент больше минимального в куче – заменяем корень и просеиваем вниз
            heap[0] = similarities[i];
            int idx = 0;
            // Sift-down
            while (1) {
                int smallest = idx;
                int left = LEFT(idx);
                int right = RIGHT(idx);
                if (left < heap_size && heap[left].similarity < heap[smallest].similarity)
                    smallest = left;
                if (right < heap_size && heap[right].similarity < heap[smallest].similarity)
                    smallest = right;
                if (smallest != idx) {
                    SimilarityResult tmp = heap[idx];
                    heap[idx] = heap[smallest];
                    heap[smallest] = tmp;
                    idx = smallest;
                } else {
                    break;
                }
            }
        }
    }

    // Теперь в куче – topK элементов (неупорядоченно).
    // Отсортируем их по убыванию similarity, чтобы первые были самыми похожими.
    for (int i = 0; i < heap_size - 1; i++) {
        int best = i;
        for (int j = i + 1; j < heap_size; j++) {
            if (heap[j].similarity > heap[best].similarity)
                best = j;
        }
        if (best != i) {
            SimilarityResult tmp = heap[i];
            heap[i] = heap[best];
            heap[best] = tmp;
        }
    }

    // Копируем итоговые topK идентификаторов в results
    int count = heap_size; // <= topK
    for (int i = 0; i < count; i++) {
        results[i] = heap[i].node_id;
    }

    free(heap);
    // ---- Конец замены ----

    free(similarities);
    return count;
}
