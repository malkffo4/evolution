// storage/vector_store/vector_store.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <lmdb.h>

#include "storage/db/db.h"
#include "storage/vector_store/vector_store.h"
#include "runtime/logging/logging.h"

// Xoshiro256** PRNG
static uint64_t xs_state[4];
static inline uint64_t xs_rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
static uint64_t xs_next(void) {
    uint64_t result = xs_rotl(xs_state[1] * 5, 7) * 9;
    uint64_t t = xs_state[1] << 17;
    xs_state[2] ^= xs_state[0]; xs_state[3] ^= xs_state[1];
    xs_state[1] ^= xs_state[2]; xs_state[0] ^= xs_state[3];
    xs_state[2] ^= t; xs_state[3] = xs_rotl(xs_state[3], 45);
    return result;
}
static void xs_seed(uint64_t seed) {
    for (int i = 0; i < 4; i++) {
        uint64_t z = (seed += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        xs_state[i] = z ^ (z >> 31);
    }
}
static inline float xs_float(void) {
    uint32_t bits = (uint32_t)(xs_next() >> 40);
    float f = (float)bits / (float)(1ULL << 23);
    return f * 2.0f - 1.0f;
}

const char *config_key = "proj_matrix";
static float proj_matrix[HASH_BITS][VECTOR_DIM];

int init_simhash(MDB_txn *txn) {
    MDB_val key = { strlen(config_key) + 1, (void*)config_key };
    MDB_val data;

    if (mdb_get(txn, db.vectors.simhash_config, &key, &data) == MDB_SUCCESS) {
        if (data.mv_size == sizeof(proj_matrix)) {
            memcpy(proj_matrix, data.mv_data, sizeof(proj_matrix));
            return 0;
        }
    }

    LOG_DATABASE("Generating new SimHash projection matrix...");
    xs_seed(42);
    for (int i = 0; i < HASH_BITS; i++) {
        for (int j = 0; j < VECTOR_DIM; j++) {
            proj_matrix[i][j] = xs_float();
        }
    }

    data.mv_size = sizeof(proj_matrix);
    data.mv_data = proj_matrix;
    return mdb_put(txn, db.vectors.simhash_config, &key, &data, 0);
}

void compute_simhash256(const float *embedding, uint64_t hash[4]) {
    memset(hash, 0, 4 * sizeof(uint64_t));
    for (int i = 0; i < HASH_BITS; i++) {
        float sum = 0.0f;
        for (int j = 0; j < VECTOR_DIM; j++) {
            sum += embedding[j] * proj_matrix[i][j];
        }
        if (sum > 0) {
            hash[i / 64] |= (1ULL << (i % 64));
        }
    }
}

// Читаем из единой таблицы гипер-графа idx_vectors
int load_embedding(MDB_txn *txn, ko_id_t node_id, float *emb_out) {
    MDB_val key = { sizeof(ko_id_t), &node_id };
    MDB_val data;
    if (mdb_get(txn, db.graph.hyper.idx_vectors, &key, &data) != MDB_SUCCESS) return -1;
    if (data.mv_size != VECTOR_DIM * sizeof(float)) return -1;
    memcpy(emb_out, data.mv_data, data.mv_size);
    return 0;
}

// Записываем в единую таблицу гипер-графа idx_vectors + обновляем SimHash
int save_embedding(MDB_txn *txn, ko_id_t node_id, const float *emb) {
    MDB_val key = { sizeof(ko_id_t), &node_id };
    MDB_val data = { VECTOR_DIM * sizeof(float), (void *)emb };
    int rc = mdb_put(txn, db.graph.hyper.idx_vectors, &key, &data, 0);
    if (rc != MDB_SUCCESS) return rc;

    uint64_t simhash[4];
    compute_simhash256(emb, simhash);

    key.mv_size = sizeof(simhash);
    key.mv_data = simhash;
    data.mv_size = sizeof(ko_id_t);
    data.mv_data = &node_id;
    // Используем MDB_APPENDDUP (или 0), так как база открыта как MDB_DUPSORT
    return mdb_put(txn, db.vectors.simhash_index, &key, &data, 0);
}

static float cosine_similarity(const float *a, const float *b, int dim) {
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i]; norm_a += a[i] * a[i]; norm_b += b[i] * b[i];
    }
    if (norm_a < 1e-8f || norm_b < 1e-8f) return 0.0f;
    return dot / (sqrtf(norm_a) * sqrtf(norm_b));
}

static int hamming_distance(const uint64_t *a, const uint64_t *b) {
    int dist = 0;
    for (int i = 0; i < 4; i++) dist += __builtin_popcountll(a[i] ^ b[i]);
    return dist;
}

typedef struct { ko_id_t node_id; float similarity; } SimilarityResult;

int find_similar_nodes(MDB_txn *txn, const float *query_emb, int topK, uint64_t *results) {
    uint64_t query_hash[4];
    compute_simhash256(query_emb, query_hash);

    MDB_cursor *cursor;
    if (mdb_cursor_open(txn, db.vectors.simhash_index, &cursor) != MDB_SUCCESS) return 0;

    ko_id_t candidates[MAX_CANDIDATES];
    int candidate_count = 0;

    MDB_val key, data;
    int rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
    while (rc == MDB_SUCCESS && candidate_count < MAX_CANDIDATES) {
        if (key.mv_size == sizeof(query_hash)) {
            uint64_t cand_hash[4];
            memcpy(cand_hash, key.mv_data, sizeof(cand_hash));
            int dist = hamming_distance(cand_hash, query_hash);
            if (dist < HAMMING_THRESHOLD) {
                do {
                    if (data.mv_size == sizeof(ko_id_t)) {
                        ko_id_t cand_id;
                        memcpy(&cand_id, data.mv_data, sizeof(ko_id_t));
                        candidates[candidate_count++] = cand_id;
                    }
                    rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_DUP);
                } while (rc == MDB_SUCCESS && candidate_count < MAX_CANDIDATES);
                rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_NODUP);
                continue;
            }
        }
        rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT);
    }
    mdb_cursor_close(cursor);

    if (candidate_count == 0) return 0;

    SimilarityResult *similarities = malloc((size_t)candidate_count * sizeof(SimilarityResult));
    if (!similarities)
        goto cleanup;
    int valid_count = 0;
    for (int i = 0; i < candidate_count; i++) {
        float emb[VECTOR_DIM];
        if (load_embedding(txn, candidates[i], emb) == 0) {
            similarities[valid_count].node_id = candidates[i];
            similarities[valid_count].similarity = cosine_similarity(query_emb, emb, VECTOR_DIM);
            valid_count++;
        }
    }

    if (valid_count == 0) { free(similarities); return 0; }

    int heap_capacity = (topK < valid_count) ? topK : valid_count;
    SimilarityResult *heap = malloc((size_t)heap_capacity * sizeof(SimilarityResult));
    if (!heap)
        goto cleanup;
    int heap_size = 0;

    #define PARENT(i) (((i) - 1) / 2)
    #define LEFT(i)   (2 * (i) + 1)
    #define RIGHT(i)  (2 * (i) + 2)

    for (int i = 0; i < valid_count; i++) {
        if (heap_size < heap_capacity) {
            int idx = heap_size++;
            heap[idx] = similarities[i];
            while (idx > 0 && heap[idx].similarity < heap[PARENT(idx)].similarity) {
                SimilarityResult tmp = heap[idx];
                heap[idx] = heap[PARENT(idx)];
                heap[PARENT(idx)] = tmp;
                idx = PARENT(idx);
            }
        } else if (similarities[i].similarity > heap[0].similarity) {
            heap[0] = similarities[i];
            int idx = 0;
            while (1) {
                int smallest = idx, left = LEFT(idx), right = RIGHT(idx);
                if (left < heap_size && heap[left].similarity < heap[smallest].similarity)
                    smallest = left;
                if (right < heap_size && heap[right].similarity < heap[smallest].similarity)
                    smallest = right;
                if (smallest != idx) {
                    SimilarityResult tmp = heap[idx];
                    heap[idx] = heap[smallest];
                    heap[smallest] = tmp;
                    idx = smallest;
                } else
                    break;
            }
        }
    }

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

    for (int i = 0; i < heap_size; i++)
        results[i] = heap[i].node_id;

    if (heap)
        free(heap);
    if (similarities)
        free(similarities);
    return heap_size;
cleanup:
    if (heap)
        free(heap);
    if (similarities)
        free(similarities);
    return -1;
}
