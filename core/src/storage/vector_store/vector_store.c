#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <lmdb.h>

#include "storage/db/db.h"
#include "storage/vector_store/vector_store.h"

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
        // Matrix exists in DB, load it
        if (data.mv_size != sizeof(proj_matrix)) {
            fprintf(stderr, "Invalid projection matrix size in DB\n");
            return -1;
        }
        memcpy(proj_matrix, data.mv_data, sizeof(proj_matrix));
        printf("Loaded projection matrix from DB\n");
        return 0;
    }

    // Matrix doesn't exist, generate new one
    printf("Generating new projection matrix...\n");
    srand(42); // Fixed seed for reproducibility
    for (int i = 0; i < HASH_BITS; i++) {
        for (int j = 0; j < EMBEDDING_DIM; j++) {
            proj_matrix[i][j] = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        }
    }

    // Save to DB
    data.mv_size = sizeof(proj_matrix);
    data.mv_data = proj_matrix;
    rc = mdb_put(txn, db.vectors.simhash_config, &key, &data, 0);
    if (rc != MDB_SUCCESS) {
        fprintf(stderr, "Failed to save projection matrix: %s\n", mdb_strerror(rc));
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
    const int HAMMING_THRESHOLD = 30;
    uint64_t candidates[1000]; // Max candidates
    int candidate_count = 0;

    // Iterate through all SimHash entries
    rc = mdb_cursor_get(cursor, &key, &data, MDB_FIRST);
    while (rc == MDB_SUCCESS && candidate_count < 1000) {
        if (key.mv_size == sizeof(query_hash)) {
            int dist = hamming_distance((uint64_t *)key.mv_data, query_hash);
            if (dist < HAMMING_THRESHOLD) {
                // This is a candidate, add all node_ids for this SimHash
                do {
                    if (data.mv_size == sizeof(uint64_t)) {
                        candidates[candidate_count++] = *(uint64_t *)data.mv_data;
                    }
                    rc = mdb_cursor_get(cursor, &key, &data, MDB_NEXT_DUP);
                } while (rc == MDB_SUCCESS && candidate_count < 1000);
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

    SimilarityResult *similarities = malloc(candidate_count * sizeof(SimilarityResult));
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

    // Sort by similarity (descending)
    for (int i = 0; i < valid_count - 1; i++) {
        for (int j = i + 1; j < valid_count; j++) {
            if (similarities[i].similarity < similarities[j].similarity) {
                SimilarityResult temp = similarities[i];
                similarities[i] = similarities[j];
                similarities[j] = temp;
            }
        }
    }

    // Return top K results
    int count = (topK < valid_count) ? topK : valid_count;
    for (int i = 0; i < count; i++) {
        results[i] = similarities[i].node_id;
    }

    free(similarities);
    return count;
}
