// storage/vector_store/vector_store.h
#ifndef VECTOR_STORE_H
#define VECTOR_STORE_H

#include <stdint.h>
#include <lmdb.h>

#define HASH_BITS           256
#define EMBEDDING_DIM       768

#define HAMMING_THRESHOLD   30
#define MAX_CANDIDATES      1000

// Initialize the SimHash projection matrix (load from DB or generate)
int init_simhash(MDB_txn *txn);

// Compute 256-bit semantic hash from embedding
void compute_simhash256(const float *embedding, uint64_t hash[4]);

// Save embedding and update SimHash index
int save_embedding(MDB_txn *txn, uint64_t node_id, const float *emb);

// Load embedding for a node
int load_embedding(MDB_txn *txn, uint64_t node_id, float *emb_out);

// Find similar nodes using SimHash + cosine similarity
int find_similar_nodes(MDB_txn *txn, const float *query_emb, int topK, uint64_t *results);

#endif // VECTOR_STORE_H
